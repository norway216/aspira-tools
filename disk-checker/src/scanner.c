#include "scanner.h"
#include "logger.h"

#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/hdreg.h>
#include <errno.h>
#include <ctype.h>

/* ============================================================
 *  Pattern matching helpers
 * ============================================================ */

/* Match /dev/sd[a-z] — returns 1 if valid, stores basename in name */
static int is_sata_device(const char *d_name, char *name, size_t name_sz) {
    /* Must be exactly "sd" + single letter */
    if (strncmp(d_name, "sd", 2) != 0) return 0;
    const char *p = d_name + 2;
    if (!isalpha((unsigned char)*p)) return 0;
    /* Check no trailing digits (sdX, not sdX1) */
    const char *q = p + 1;
    while (*q) {
        if (isalpha((unsigned char)*q) || isdigit((unsigned char)*q)) {
            /* This is a partition, not a whole disk */
            if (isdigit((unsigned char)*q)) return 0;
        } else {
            return 0;
        }
        q++;
    }
    snprintf(name, name_sz, "%s", d_name);
    return 1;
}

/* Match /dev/nvme[0-9]n[0-9] — whole NVMe device, not partition */
static int is_nvme_device(const char *d_name, char *name, size_t name_sz) {
    if (strncmp(d_name, "nvme", 4) != 0) return 0;
    const char *p = d_name + 4;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    if (*p != 'n') return 0;
    p++;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    /* If there's anything left (like "p1"), it's a partition */
    if (*p != '\0') return 0;
    snprintf(name, name_sz, "%s", d_name);
    return 1;
}

/* ============================================================
 *  SATA device identity (HDIO_GET_IDENTITY)
 * ============================================================ */

static int read_sata_identity(const char *dev_path, device_info_t *info) {
    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        logger_write(LOG_DEBUG, "Cannot open %s: %s", dev_path, strerror(errno));
        return -1;
    }

    struct hd_driveid id;
    memset(&id, 0, sizeof(id));

    if (ioctl(fd, HDIO_GET_IDENTITY, &id) < 0) {
        logger_write(LOG_DEBUG, "HDIO_GET_IDENTITY failed for %s: %s",
                     dev_path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    /* Copy and trim model (40 chars, no null terminator in struct) */
    memcpy(info->model, id.model, 40);
    info->model[40] = '\0';
    str_trim_tail(info->model);

    /* Copy and trim serial (20 chars) */
    memcpy(info->serial, id.serial_no, 20);
    info->serial[20] = '\0';
    str_trim_tail(info->serial);

    /* Copy and trim firmware revision (8 chars) */
    memcpy(info->fw_rev, id.fw_rev, 8);
    info->fw_rev[8] = '\0';
    str_trim_tail(info->fw_rev);

    return 0;
}

/* ============================================================
 *  NVMe device identity (sysfs)
 * ============================================================ */

static int read_nvme_identity(const char *dev_name, device_info_t *info) {
    char sys_path[128];

    /* Model */
    snprintf(sys_path, sizeof(sys_path),
             "/sys/class/nvme/%s/device/model", dev_name);
    FILE *f = fopen(sys_path, "r");
    if (f) {
        if (fgets(info->model, DEV_MODEL_MAX, f)) {
            str_trim_tail(info->model);
        }
        fclose(f);
    }

    /* Serial */
    snprintf(sys_path, sizeof(sys_path),
             "/sys/class/nvme/%s/device/serial", dev_name);
    f = fopen(sys_path, "r");
    if (f) {
        if (fgets(info->serial, DEV_SERIAL_MAX, f)) {
            str_trim_tail(info->serial);
        }
        fclose(f);
    }

    /* Firmware */
    snprintf(sys_path, sizeof(sys_path),
             "/sys/class/nvme/%s/device/firmware_rev", dev_name);
    f = fopen(sys_path, "r");
    if (f) {
        if (fgets(info->fw_rev, DEV_FW_REV_MAX, f)) {
            str_trim_tail(info->fw_rev);
        }
        fclose(f);
    }

    return 0;
}

/* ============================================================
 *  Rotational check (HDD vs SSD)
 * ============================================================ */

static int check_rotational(const char *dev_name) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", dev_name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;  /* unknown */

    char buf[8] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return (buf[0] == '1') ? 1 : 0;
    }
    fclose(f);
    return -1;
}

/* ============================================================
 *  Public API
 * ============================================================ */

int scan_block_devices(device_list_t *list) {
    memset(list, 0, sizeof(*list));

    DIR *dir = opendir("/dev");
    if (!dir) {
        logger_write(LOG_CRITICAL, "Cannot open /dev: %s", strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && list->count < MAX_DEVICES) {
        char dev_name[DEV_NAME_MAX] = {0};
        char dev_path[DEV_PATH_MAX] = {0};
        device_info_t *info = &list->devices[list->count];

        if (is_sata_device(entry->d_name, dev_name, sizeof(dev_name))) {
            snprintf(dev_path, DEV_PATH_MAX, "/dev/%s", dev_name);
            snprintf(info->path, DEV_PATH_MAX, "%s", dev_path);
            snprintf(info->name, DEV_NAME_MAX, "%s", dev_name);
            info->type = DEVICE_TYPE_SATA;

            if (read_sata_identity(dev_path, info) == 0) {
                logger_write(LOG_DEBUG, "SATA: %s model=%s serial=%s",
                             dev_path, info->model, info->serial);
            }
            info->rotational = check_rotational(dev_name);
            list->count++;

        } else if (is_nvme_device(entry->d_name, dev_name, sizeof(dev_name))) {
            snprintf(dev_path, DEV_PATH_MAX, "/dev/%s", dev_name);
            snprintf(info->path, DEV_PATH_MAX, "%s", dev_path);
            snprintf(info->name, DEV_NAME_MAX, "%s", dev_name);
            info->type = DEVICE_TYPE_NVME;

            (void)read_nvme_identity(dev_name, info);
            info->rotational = 0;  /* NVMe is always SSD */
            logger_write(LOG_DEBUG, "NVMe: %s model=%s", dev_path, info->model);
            list->count++;
        }
    }

    closedir(dir);
    return list->count;
}

int classify_device(const char *dev_path, device_info_t *info) {
    if (!dev_path || !info) return -1;

    /* Extract basename */
    const char *basename = strrchr(dev_path, '/');
    if (basename) basename++; else basename = dev_path;

    if (strncmp(basename, "sd", 2) == 0) {
        info->type = DEVICE_TYPE_SATA;
    } else if (strncmp(basename, "nvme", 4) == 0) {
        info->type = DEVICE_TYPE_NVME;
    } else {
        info->type = DEVICE_TYPE_UNKNOWN;
        return -1;
    }

    snprintf(info->path, DEV_PATH_MAX, "%s", dev_path);
    snprintf(info->name, DEV_NAME_MAX, "%s", basename);
    info->rotational = check_rotational(basename);
    return 0;
}

int get_device_info(const char *dev_path, device_info_t *info) {
    if (!dev_path || !info) return -1;
    memset(info, 0, sizeof(*info));

    if (classify_device(dev_path, info) != 0) return -1;

    if (info->type == DEVICE_TYPE_SATA) {
        return read_sata_identity(dev_path, info);
    } else if (info->type == DEVICE_TYPE_NVME) {
        return read_nvme_identity(info->name, info);
    }
    return -1;
}
