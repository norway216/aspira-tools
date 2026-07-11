/**
 * @file op_light_install.c
 * @brief Lightweight system installation – selective partition flash.
 */

#include "op_interface.h"
#include "common/utils.h"
#include <stdio.h>
#include <string.h>

#define SCRIPT "/scripts/init-bottom/zz-jelina_flasher"

typedef struct {
    int  pct;
    const char *desc;
    const char *func;
} step_t;

static const step_t steps[] = {
    {10,  "Initializing...",          "part_update_init"},
    {20,  "Verifying checksum...",    "verify_and_extract"},
    {30,  "Updating flash...",        "part_update_flash"},
    {40,  "Updating boot...",         "part_update_boot"},
    {50,  "Updating recovery...",     "part_update_recovery"},
    {60,  "Updating kernel...",       "part_update_kernel"},
    {70,  "Updating rootfs...",       "part_update_root"},
    {80,  "Updating data...",         "part_update_data"},
    {90,  "Copying recovery data...", "copy_recovery_data"},
    {100, "Finalizing...",            "part_fatlabel_data"},
};

static int op_validate(void)
{
    if (!utils_file_exists(SCRIPT)) {
        fprintf(stderr, "light_install: script not found\n");
        return -1;
    }
    return 0;
}

static int op_init(void) { return 0; }

static operation_result_t op_execute(progress_callback_t progress, void *ctx)
{
    (void)ctx;
    operation_result_t result = { .success = false, .error_code = -1 };
    int total = (int)(sizeof(steps) / sizeof(steps[0]));

    for (int i = 0; i < total; i++) {
        if (progress) progress(steps[i].pct, steps[i].desc, NULL);

        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s %s", SCRIPT, steps[i].func);

        if (utils_shell_exec(cmd) != 0) {
            snprintf(result.message, sizeof(result.message),
                     "Failed at step: %s", steps[i].desc);
            return result;
        }
    }

    if (progress) progress(100, "Installation complete", NULL);

    result.success = true;
    result.error_code = 0;
    snprintf(result.message, sizeof(result.message),
             "Lightweight installation completed successfully");
    return result;
}

static void op_cleanup(void) { }

__attribute__((constructor))
static void register_plugin(void)
{
    static operation_plugin_t plugin = {
        .name        = "light_install",
        .description = "Lightweight System Installation",
        .validate    = op_validate,
        .init        = op_init,
        .execute     = op_execute,
        .cleanup     = op_cleanup,
    };
    operation_plugin_register(&plugin);
}
