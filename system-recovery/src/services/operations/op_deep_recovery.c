/**
 * @file op_deep_recovery.c
 * @brief Deep system recovery – full reflash of kernel and rootfs partitions.
 */

#include "op_interface.h"
#include "hal/storage/storage.h"
#include "common/utils.h"
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

    /* 1. Clear U-Boot environment */
    if (progress) progress(5, "Clearing boot environment...", NULL);
    if (utils_shell_exec("dd if=/dev/zero of=/dev/mmcblk0 bs=1K count=8 seek=4064") != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to clear ENV section");
        return result;
    }

    /* 2. Mount recovery partition */
    if (progress) progress(10, "Mounting recovery partition...", NULL);
    if (storage_mount(&recovery_mp) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to mount recovery partition");
        return result;
    }

    /* 3. Verify checksums */
    if (progress) progress(15, "Verifying kernel checksum...", NULL);
    if (!utils_verify_md5("/tmp/sr_recovery/kernela.img")) {
        snprintf(result.message, sizeof(result.message), "Kernel MD5 verification failed");
        storage_umount(&recovery_mp);
        return result;
    }

    if (progress) progress(25, "Verifying rootfs checksum...", NULL);
    if (!utils_verify_md5("/tmp/sr_recovery/roota.tar.gz")) {
        snprintf(result.message, sizeof(result.message), "Rootfs MD5 verification failed");
        storage_umount(&recovery_mp);
        return result;
    }

    /* 4. Flash kernel A */
    if (progress) progress(35, "Flashing kernel A...", NULL);
    utils_shell_exec("partprobe /dev/mmcblk0");
    utils_shell_exec("mkfs.ext4 -F /dev/mmcblk0p3");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "dd if=/tmp/sr_recovery/kernela.img of=/dev/mmcblk0p3 bs=4M");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to flash kernel A");
        storage_umount(&recovery_mp);
        return result;
    }

    /* 5. Flash kernel B */
    if (progress) progress(45, "Flashing kernel B...", NULL);
    utils_shell_exec("mkfs.ext4 -F /dev/mmcblk0p4");
    snprintf(cmd, sizeof(cmd), "dd if=/tmp/sr_recovery/kernela.img of=/dev/mmcblk0p4 bs=4M");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to flash kernel B");
        storage_umount(&recovery_mp);
        return result;
    }

    /* 6. Flash rootfs A */
    if (progress) progress(55, "Flashing rootfs A...", NULL);
    utils_shell_exec("mkfs.ext4 -F /dev/mmcblk0p5");
    if (storage_mount(&root_a_mp) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to mount rootfs A");
        storage_umount(&recovery_mp);
        return result;
    }
    snprintf(cmd, sizeof(cmd), "tar -xzf /tmp/sr_recovery/roota.tar.gz -C /tmp/sr_roota/");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to extract rootfs A");
        storage_umount(&root_a_mp);
        storage_umount(&recovery_mp);
        return result;
    }
    storage_umount(&root_a_mp);

    /* 7. Flash rootfs B */
    if (progress) progress(70, "Flashing rootfs B...", NULL);
    utils_shell_exec("mkfs.ext4 -F /dev/mmcblk0p6");
    if (storage_mount(&root_b_mp) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to mount rootfs B");
        storage_umount(&recovery_mp);
        return result;
    }
    snprintf(cmd, sizeof(cmd), "tar -xzf /tmp/sr_recovery/roota.tar.gz -C /tmp/sr_rootb/");
    if (utils_shell_exec(cmd) != 0) {
        snprintf(result.message, sizeof(result.message), "Failed to extract rootfs B");
        storage_umount(&root_b_mp);
        storage_umount(&recovery_mp);
        return result;
    }
    storage_umount(&root_b_mp);
    storage_umount(&recovery_mp);

    /* 8. Clean data partition overlay */
    if (progress) progress(85, "Cleaning data partition...", NULL);
    if (storage_mount(&data_mp) == 0) {
        snprintf(cmd, sizeof(cmd),
                 "cd /tmp/sr_data/root-0/upper && "
                 "find . -maxdepth 1 ! -name '.' ! -name 'harddisk' ! -name 'usr' "
                 "-exec rm -rf {} \\; && "
                 "cd usr && find . -maxdepth 1 ! -name '.' ! -name 'tigerapp' "
                 "-exec rm -rf {} \\;");
        utils_shell_exec(cmd);
        storage_umount(&data_mp);
    }

    if (progress) progress(100, "Deep recovery complete", NULL);

    utils_shell_exec("sync");

    result.success = true;
    result.error_code = 0;
    snprintf(result.message, sizeof(result.message),
             "Deep system recovery completed successfully");
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
