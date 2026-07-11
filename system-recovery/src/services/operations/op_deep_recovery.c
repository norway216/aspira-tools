/**
 * @file op_deep_recovery.c
 * @brief Deep system recovery – full reflash of kernel and rootfs partitions.
 */

#include "op_interface.h"
#include "hal/storage/storage.h"
#include "common/utils.h"
#include "services/service_manager.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static mount_point_t recovery_mp;
static mount_point_t data_mp;
static mount_point_t root_a_mp;
static mount_point_t root_b_mp;

static int op_validate(void)
{
    if (!storage_device_exists("/dev/mmcblk0p2")) {
        fprintf(stderr, "deep_recovery: recovery partition not found\n");
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

    memset(&root_a_mp, 0, sizeof(root_a_mp));
    root_a_mp.mount_path = "/tmp/sr_roota";
    root_a_mp.device     = "/dev/mmcblk0p5";
    root_a_mp.fs_type    = "ext4";

    memset(&root_b_mp, 0, sizeof(root_b_mp));
    root_b_mp.mount_path = "/tmp/sr_rootb";
    root_b_mp.device     = "/dev/mmcblk0p6";
    root_b_mp.fs_type    = "ext4";

    return 0;
}

static operation_result_t op_execute(progress_callback_t progress, void *ctx)
{
    (void)ctx;
    operation_result_t result = { .success = false, .error_code = -1 };
    bool recovery_mounted = false;
    bool root_a_mounted = false;
    bool root_b_mounted = false;
    bool data_mounted = false;
    char cmd[512];

#define CHECK_CANCEL do { \
    if (service_manager_cancelled()) { \
        snprintf(result.message, sizeof(result.message), "Operation cancelled by user"); \
        goto cleanup; \
    } \
} while(0)

    /* 1. Clear U-Boot environment */
    if (progress) progress(5, "Clearing boot environment...", NULL);
    if (utils_shell_exec("dd if=/dev/zero of=/dev/mmcblk0 bs=1K count=8 seek=4064") != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to clear ENV section");
        goto cleanup;
    }
    CHECK_CANCEL;

    /* 2. Mount recovery partition */
    if (progress) progress(10, "Mounting recovery partition...", NULL);
    if (storage_mount(&recovery_mp) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to mount recovery partition");
        goto cleanup;
    }
    recovery_mounted = true;
    CHECK_CANCEL;

    /* 3. Verify checksums */
    if (progress) progress(15, "Verifying kernel checksum...", NULL);
    if (!utils_verify_md5("/tmp/sr_recovery/kernela.img")) {
        snprintf(result.message, sizeof(result.message), "Kernel MD5 verification failed");
        goto cleanup;
    }
    CHECK_CANCEL;

    if (progress) progress(25, "Verifying rootfs checksum...", NULL);
    if (!utils_verify_md5("/tmp/sr_recovery/roota.tar.gz")) {
        snprintf(result.message, sizeof(result.message), "Rootfs MD5 verification failed");
        goto cleanup;
    }
    CHECK_CANCEL;

    /* 4. Flash kernel A */
    if (progress) progress(35, "Flashing kernel A...", NULL);
    if (utils_shell_exec("partprobe /dev/mmcblk0") != 0) {
        fprintf(stderr, "deep_recovery: partprobe warning (continuing)\n");
    }
    if (utils_shell_exec("mkfs.ext4 -F /dev/mmcblk0p3") != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to format kernel A partition");
        goto cleanup;
    }
    snprintf(cmd, sizeof(cmd), "dd if=/tmp/sr_recovery/kernela.img of=/dev/mmcblk0p3 bs=4M");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to flash kernel A");
        goto cleanup;
    }
    CHECK_CANCEL;

    /* 5. Flash kernel B */
    if (progress) progress(45, "Flashing kernel B...", NULL);
    if (utils_shell_exec("mkfs.ext4 -F /dev/mmcblk0p4") != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to format kernel B partition");
        goto cleanup;
    }
    snprintf(cmd, sizeof(cmd), "dd if=/tmp/sr_recovery/kernela.img of=/dev/mmcblk0p4 bs=4M");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to flash kernel B");
        goto cleanup;
    }
    CHECK_CANCEL;

    /* 6. Flash rootfs A */
    if (progress) progress(55, "Flashing rootfs A...", NULL);
    if (utils_shell_exec("mkfs.ext4 -F /dev/mmcblk0p5") != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to format rootfs A partition");
        goto cleanup;
    }
    if (storage_mount(&root_a_mp) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to mount rootfs A");
        goto cleanup;
    }
    root_a_mounted = true;
    snprintf(cmd, sizeof(cmd), "tar -xzf /tmp/sr_recovery/roota.tar.gz -C /tmp/sr_roota/");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to extract rootfs A");
        goto cleanup;
    }
    storage_umount(&root_a_mp);
    root_a_mounted = false;
    CHECK_CANCEL;

    /* 7. Flash rootfs B */
    if (progress) progress(70, "Flashing rootfs B...", NULL);
    if (utils_shell_exec("mkfs.ext4 -F /dev/mmcblk0p6") != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to format rootfs B partition");
        goto cleanup;
    }
    if (storage_mount(&root_b_mp) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to mount rootfs B");
        goto cleanup;
    }
    root_b_mounted = true;
    snprintf(cmd, sizeof(cmd), "tar -xzf /tmp/sr_recovery/roota.tar.gz -C /tmp/sr_rootb/");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to extract rootfs B");
        goto cleanup;
    }
    storage_umount(&root_b_mp);
    root_b_mounted = false;

    /* 8. Clean data partition overlay */
    CHECK_CANCEL;
    if (progress) progress(85, "Cleaning data partition...", NULL);
    if (storage_mount(&data_mp) == 0) {
        data_mounted = true;
        snprintf(cmd, sizeof(cmd),
                 "cd /tmp/sr_data/root-0/upper && "
                 "find . -maxdepth 1 ! -name '.' ! -name 'harddisk' ! -name 'usr' "
                 "-exec rm -rf {} \\; && "
                 "cd usr && find . -maxdepth 1 ! -name '.' ! -name 'tigerapp' "
                 "-exec rm -rf {} \\;");
        utils_shell_exec(cmd);
        storage_umount(&data_mp);
        data_mounted = false;
    }

    if (progress) progress(100, "Deep recovery complete", NULL);

    utils_shell_exec("sync");

    result.success = true;
    result.error_code = 0;
    snprintf(result.message, sizeof(result.message),
             "Deep system recovery completed successfully");

cleanup:
    /* Always unmount any still-mounted partitions */
    if (data_mounted)   storage_umount(&data_mp);
    if (root_b_mounted) storage_umount(&root_b_mp);
    if (root_a_mounted) storage_umount(&root_a_mp);
    if (recovery_mounted) {
        storage_umount(&recovery_mp);
        if (!result.success) {
            /* Ensure recovery partition is cleanly unmounted on failure */
        }
    }

#undef CHECK_CANCEL
    return result;
}

static void op_cleanup(void)
{
    storage_umount(&recovery_mp);
    storage_umount(&data_mp);
    storage_umount(&root_a_mp);
    storage_umount(&root_b_mp);
}

__attribute__((constructor))
static void register_plugin(void)
{
    static operation_plugin_t plugin = {
        .name        = "deep_recovery",
        .description = "Deep System Recovery (Full Reflash)",
        .validate    = op_validate,
        .init        = op_init,
        .execute     = op_execute,
        .cleanup     = op_cleanup,
    };
    operation_plugin_register(&plugin);
}
