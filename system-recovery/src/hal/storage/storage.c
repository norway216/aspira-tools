#include "storage.h"
#include "common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ---- Mount / Umount ---------------------------------------------------- */

int storage_mount(mount_point_t *mp)
{
    if (mp == NULL) return -1;

    struct stat st;
    if (stat(mp->mount_path, &st) == -1) {
        if (utils_mkdir_p(mp->mount_path) != 0) {
            fprintf(stderr, "storage: cannot create %s\n", mp->mount_path);
            return -1;
        }
        mp->dir_created = true;
    }

    char cmd[512];
    if (mp->fs_type != NULL) {
        snprintf(cmd, sizeof(cmd), "mount -t %s %s %s",
                 mp->fs_type, mp->device, mp->mount_path);
    } else {
        snprintf(cmd, sizeof(cmd), "mount %s %s",
                 mp->device, mp->mount_path);
    }

    if (utils_shell_exec(cmd) != 0) {
        fprintf(stderr, "storage: mount failed: %s\n", cmd);
        if (mp->dir_created) {
            rmdir(mp->mount_path);
            mp->dir_created = false;
        }
        return -1;
    }

    mp->is_mounted = true;
    return 0;
}

int storage_umount(mount_point_t *mp)
{
    if (mp == NULL) return -1;

    if (mp->is_mounted) {
        umount(mp->mount_path);
        mp->is_mounted = false;
    }

    if (mp->dir_created) {
        rmdir(mp->mount_path);
        mp->dir_created = false;
    }

    return 0;
}

/* ---- External Device --------------------------------------------------- */

bool storage_device_exists(const char *device)
{
    if (device == NULL) return false;
    return utils_file_exists(device);
}

const char *storage_get_external_device(void)
{
    /* USB disk takes priority; fall back to SD card. */
    if (storage_device_exists("/dev/sda1")) {
        return "/dev/sda1";
    }
    if (storage_device_exists("/dev/mmcblk1p1")) {
        return "/dev/mmcblk1p1";
    }
    return NULL;
}

/* ---- Version Info ------------------------------------------------------ */

int storage_read_version(const char *mount_path, version_info_t *info)
{
    if (mount_path == NULL || info == NULL) return -1;

    memset(info, 0, sizeof(*info));

    char ini_path[512];
    snprintf(ini_path, sizeof(ini_path), "%s/version.ini", mount_path);

    FILE *fp = fopen(ini_path, "r");
    if (fp == NULL) {
        /* Try root subdirectory */
        snprintf(ini_path, sizeof(ini_path), "%s/root/version.ini", mount_path);
        fp = fopen(ini_path, "r");
        if (fp == NULL) return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        /* Also strip carriage return */
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        if (strncmp(line, "Model=", 6) == 0) {
            strncpy(info->model, line + 6, sizeof(info->model) - 1);
        } else if (strncmp(line, "SoftwareVersion=", 16) == 0) {
            strncpy(info->version, line + 16, sizeof(info->version) - 1);
        }
    }
    fclose(fp);

    return (info->version[0] != '\0') ? 0 : -1;
}
