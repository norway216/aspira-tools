/**
 * @file op_app_backup.c
 * @brief Application software backup – copies app data from local partition
 *        to the recovery partition.
 */

#include "op_interface.h"
#include "hal/storage/storage.h"
#include "common/utils.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static mount_point_t recovery_mp;
static mount_point_t data_mp;
static mount_point_t ext_mp;

static int op_validate(void)
{
    if (!storage_device_exists("/dev/mmcblk0p2")) return -1;
    const char *ext = storage_get_external_device();
    if (ext == NULL) {
        fprintf(stderr, "app_backup: no external storage found\n");
        return -1;
    }
    return 0;
}

static int op_init(void)
{
    memset(&recovery_mp, 0, sizeof(recovery_mp));
    recovery_mp.mount_path = "/tmp/sr_recovery";
    recovery_mp.device     = "/dev/mmcblk0p2";
    recovery_mp.fs_type    = "vfat";

    memset(&data_mp, 0, sizeof(data_mp));
    data_mp.mount_path = "/tmp/sr_data";
    data_mp.device     = "/dev/mmcblk0p7";
    data_mp.fs_type    = "ext4";

    memset(&ext_mp, 0, sizeof(ext_mp));
    ext_mp.mount_path = "/tmp/sr_ext";
    ext_mp.device     = storage_get_external_device();
    ext_mp.fs_type    = "vfat";

    return 0;
}

static operation_result_t op_execute(progress_callback_t progress, void *ctx)
{
    (void)ctx;
    operation_result_t result = { .success = false, .error_code = -1 };

    /* Mount all partitions */
    if (progress) progress(5, "Mounting partitions...", NULL);
    if (storage_mount(&recovery_mp) != 0 ||
        storage_mount(&data_mp) != 0 ||
        storage_mount(&ext_mp) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to mount partitions");
        goto cleanup;
    }

    /* Read local version */
    if (progress) progress(15, "Reading version info...", NULL);
    version_info_t local_info;
    if (storage_read_version("/tmp/sr_data/root-0/upper/usr/tigerapp/res/general",
                             &local_info) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to read local version");
        goto cleanup;
    }

    /* Check external backup exists */
    if (progress) progress(25, "Checking external backup...", NULL);
    char backup_path[512];
    snprintf(backup_path, sizeof(backup_path),
             "/tmp/sr_ext/softbackup/%s/%s/backup.bin",
             local_info.model, local_info.version);
    if (!utils_file_exists(backup_path)) {
        snprintf(result.message, sizeof(result.message),
                 "Backup file not found on external media");
        goto cleanup;
    }

    /* Verify backup MD5 */
    if (progress) progress(35, "Verifying backup checksum...", NULL);
    if (!utils_verify_md5(backup_path)) {
        snprintf(result.message, sizeof(result.message),
                 "External backup MD5 verification failed");
        goto cleanup;
    }

    /* Copy to recovery partition */
    if (progress) progress(50, "Copying backup to recovery partition...", NULL);
    utils_shell_exec("rm -f /tmp/sr_recovery/backup.bin*");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cp -rf %s /tmp/sr_recovery/backup.bin", backup_path);
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to copy backup file");
        goto cleanup;
    }

    if (progress) progress(90, "Syncing filesystem...", NULL);
    utils_shell_exec("sync");

    if (progress) progress(100, "Backup complete", NULL);

    result.success = true;
    result.error_code = 0;
    snprintf(result.message, sizeof(result.message),
             "Application backup completed successfully");

cleanup:
    storage_umount(&recovery_mp);
    storage_umount(&data_mp);
    storage_umount(&ext_mp);
    return result;
}

static void op_cleanup(void)
{
    storage_umount(&recovery_mp);
    storage_umount(&data_mp);
    storage_umount(&ext_mp);
}

__attribute__((constructor))
static void register_plugin(void)
{
    static operation_plugin_t plugin = {
        .name        = "app_backup",
        .description = "Application Software Backup",
        .validate    = op_validate,
        .init        = op_init,
        .execute     = op_execute,
        .cleanup     = op_cleanup,
    };
    operation_plugin_register(&plugin);
}
