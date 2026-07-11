/**
 * @file op_light_recovery.c
 * @brief Lightweight system recovery – cleans overlay data while preserving
 *        harddisk and usr/tigerapp directories.
 */

#include "op_interface.h"
#include "hal/storage/storage.h"
#include "common/utils.h"
#include "services/service_manager.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static mount_point_t data_mp;

static int op_validate(void)
{
    if (!storage_device_exists("/dev/mmcblk0p7")) {
        fprintf(stderr, "light_recovery: data partition not found\n");
        return -1;
    }
    return 0;
}

static int op_init(void)
{
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
    bool data_mounted = false;
    char cmd[512];

    if (progress) progress(5, "Mounting data partition...", NULL);

    if (storage_mount(&data_mp) != 0) {
        snprintf(result.message, sizeof(result.message),
                 "Failed to mount data partition");
        goto cleanup;
    }
    data_mounted = true;

    if (service_manager_cancelled()) {
        snprintf(result.message, sizeof(result.message), "Operation cancelled by user");
        goto cleanup;
    }

    if (progress) progress(20, "Cleaning root overlay...", NULL);

    struct stat st;
    if (stat("/tmp/sr_data/root-0/upper", &st) == -1) {
        snprintf(result.message, sizeof(result.message),
                 "Data partition structure not found");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd),
             "cd /tmp/sr_data/root-0/upper && "
             "find . -maxdepth 1 ! -name '.' ! -name 'harddisk' ! -name 'usr' "
             "-exec rm -rf {} \\;");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message),
                 "Failed to clean root overlay");
        goto cleanup;
    }

    if (service_manager_cancelled()) {
        snprintf(result.message, sizeof(result.message), "Operation cancelled by user");
        goto cleanup;
    }

    if (progress) progress(50, "Cleaning user directory...", NULL);

    snprintf(cmd, sizeof(cmd),
             "cd /tmp/sr_data/root-0/upper/usr && "
             "find . -maxdepth 1 ! -name '.' ! -name 'tigerapp' "
             "-exec rm -rf {} \\;");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message),
                 "Failed to clean user directory");
        goto cleanup;
    }

    if (progress) progress(90, "Syncing filesystem...", NULL);
    utils_shell_exec("sync");

    if (progress) progress(100, "Light recovery complete", NULL);

    result.success = true;
    result.error_code = 0;
    snprintf(result.message, sizeof(result.message),
             "Lightweight system recovery completed successfully");

cleanup:
    if (data_mounted) storage_umount(&data_mp);
    return result;
}

static void op_cleanup(void)
{
    storage_umount(&data_mp);
}

__attribute__((constructor))
static void register_plugin(void)
{
    static operation_plugin_t plugin = {
        .name        = "light_recovery",
        .description = "Lightweight System Recovery",
        .validate    = op_validate,
        .init        = op_init,
        .execute     = op_execute,
        .cleanup     = op_cleanup,
    };
    operation_plugin_register(&plugin);
}
