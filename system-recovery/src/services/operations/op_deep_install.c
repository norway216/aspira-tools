/**
 * @file op_deep_install.c
 * @brief Full/deep system installation from USB/SD media via flasher script.
 */

#include "op_interface.h"
#include "hal/storage/storage.h"
#include "common/utils.h"
#include "services/service_manager.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define UPDATE_SCRIPT "/scripts/init-bottom/zz-jelina_flasher"
#define VARS_CONF     "/tmp/flash_vars.conf"
#define TOTAL_STEPS   12

typedef struct {
    int         start_pct;
    int         end_pct;
    const char *desc;
    const char *func;
} install_step_t;

static const install_step_t steps[] = {
    {0,   5,  "Verify Checksum",    "verify_and_extract"},
    {5,  10,  "Upgrade INIT",       "update_init"},
    {10, 15,  "Upgrade BOOT",       "update_boot"},
    {15, 20,  "Upgrade RECOVERY",   "update_recovery"},
    {20, 25,  "Upgrade KERNEL1",    "update_kernel_a"},
    {25, 35,  "Upgrade KERNEL2",    "update_kernel_b"},
    {35, 50,  "Upgrade ROOTFS1",    "update_root_a"},
    {50, 65,  "Upgrade ROOTFS2",    "update_root_b"},
    {65, 75,  "Upgrade DATA",       "update_data"},
    {75, 85,  "Change LABEL",       "update_fatlabel"},
    {85, 95,  "Copy BACKUP",        "copy_recovery_data"},
    {95, 100, "Complete",           NULL},
};

static int op_validate(void)
{
    if (!utils_file_exists(UPDATE_SCRIPT)) {
        fprintf(stderr, "deep_install: flasher script not found at %s\n", UPDATE_SCRIPT);
        return -1;
    }
    return 0;
}

static int op_init(void) { return 0; }

static operation_result_t op_execute(progress_callback_t progress, void *ctx)
{
    (void)ctx;
    operation_result_t result = { .success = false, .error_code = -1 };

    /* Validate the vars file before sourcing it from /tmp */
    bool use_vars = false;
    struct stat st;
    if (stat(VARS_CONF, &st) == 0 && S_ISREG(st.st_mode)) {
        /* Only source if owned by root and not world-writable */
        if (st.st_uid == 0 && (st.st_mode & S_IWOTH) == 0) {
            use_vars = true;
        } else {
            fprintf(stderr, "deep_install: refusing to source %s (unsafe permissions)\n",
                    VARS_CONF);
        }
    }

    for (int i = 0; i < TOTAL_STEPS; i++) {
        if (service_manager_cancelled()) {
            snprintf(result.message, sizeof(result.message), "Operation cancelled by user");
            return result;
        }

        if (steps[i].func == NULL) {
            if (progress) progress(100, "Installation complete", NULL);
            break;
        }

        if (progress) progress(steps[i].start_pct, steps[i].desc, NULL);

        char cmd[512];
        char log_file[64];
        snprintf(log_file, sizeof(log_file), "/tmp/update_step%d.log", i);

        /* Steps 0-1 run before flash_vars.conf is written; last step
         * is a no-op (func==NULL, broken above). If config is unsafe,
         * never source it — run all steps without environment. */
        if (i == 0 || i == 1 || i == (TOTAL_STEPS - 1) || !use_vars) {
            snprintf(cmd, sizeof(cmd), "%s %s > %s 2>&1",
                     UPDATE_SCRIPT, steps[i].func, log_file);
        } else {
            snprintf(cmd, sizeof(cmd), ". %s && %s %s > %s 2>&1",
                     VARS_CONF, UPDATE_SCRIPT, steps[i].func, log_file);
        }

        if (utils_shell_exec(cmd) != 0) {
            snprintf(result.message, sizeof(result.message),
                     "Failed at step: %s", steps[i].desc);
            return result;
        }
    }

    result.success = true;
    result.error_code = 0;
    snprintf(result.message, sizeof(result.message),
             "System installation completed successfully");
    return result;
}

static void op_cleanup(void) { }

REGISTER_OPERATION_PLUGIN(deep_install, "Full System Installation (from USB/SD)",
                           op_validate, op_init, op_execute, op_cleanup);
