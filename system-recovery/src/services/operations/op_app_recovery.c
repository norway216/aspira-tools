/**
 * @file op_app_recovery.c
 * @brief Application-level recovery – restores software from backup on
 *        the recovery partition to the data partition.
 */

#include "op_interface.h"
#include "hal/storage/storage.h"
#include "common/utils.h"
#include "services/service_manager.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static mount_point_t recovery_mp;
static mount_point_t data_mp;

static int op_validate(void)
{
    if (!storage_device_exists("/dev/mmcblk0p2")) return -1;
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

    return 0;
}

static operation_result_t op_execute(progress_callback_t progress, void *ctx)
{
    (void)ctx;
    operation_result_t result = { .success = false, .error_code = -1 };

    /* 1. Mount partitions */
    if (progress) progress(5, "Mounting partitions...", NULL);
    if (storage_mount(&recovery_mp) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to mount recovery partition");
        return result;
    }
    if (storage_mount(&data_mp) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to mount data partition");
        goto cleanup;
    }

    /* 2. Check backup file exists */
    if (service_manager_cancelled()) {
        snprintf(result.message, sizeof(result.message), "Operation cancelled by user");
        goto cleanup;
    }
    if (progress) progress(15, "Checking backup file...", NULL);
    if (!utils_file_exists("/tmp/sr_recovery/backup.bin")) {
        snprintf(result.message, sizeof(result.message), "Backup file not found");
        goto cleanup;
    }

    /* 3. Verify MD5 */
    if (service_manager_cancelled()) {
        snprintf(result.message, sizeof(result.message), "Operation cancelled by user");
        goto cleanup;
    }
    if (progress) progress(25, "Verifying backup checksum...", NULL);
    if (!utils_verify_md5("/tmp/sr_recovery/backup.bin")) {
        snprintf(result.message, sizeof(result.message), "Backup MD5 verification failed");
        goto cleanup;
    }

    /* 4. Extract backup */
    if (progress) progress(40, "Extracting backup...", NULL);
    const char *target = "/tmp/sr_data/root-0/upper/usr/tigerapp";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "tar -xzf /tmp/sr_recovery/backup.bin -C %s/..", target);
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to extract backup");
        goto cleanup;
    }

    if (progress) progress(90, "Syncing filesystem...", NULL);
    utils_shell_exec("sync");

    if (progress) progress(100, "App recovery complete", NULL);

    result.success = true;
    result.error_code = 0;
    snprintf(result.message, sizeof(result.message),
             "Application recovery completed successfully");

cleanup:
    storage_umount(&recovery_mp);
    storage_umount(&data_mp);
    return result;
}

static void op_cleanup(void)
{
    storage_umount(&recovery_mp);
    storage_umount(&data_mp);
}

__attribute__((constructor))
static void register_plugin(void)
{
    static operation_plugin_t plugin = {
        .name        = "app_recovery",
        .description = "Application Software Recovery",
        .validate    = op_validate,
        .init        = op_init,
        .execute     = op_execute,
        .cleanup     = op_cleanup,
    };
    operation_plugin_register(&plugin);
}
