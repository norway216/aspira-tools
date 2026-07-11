#include "ui_manager.h"
#include "core/event_bus.h"
#include "common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LVGL headers — included via lv_conf.h */
#include "lvgl/lvgl.h"

/* ---- Internal State --------------------------------------------------- */

static screen_interface_t screens[SCREEN_COUNT];
static bool               screen_registered[SCREEN_COUNT];
static screen_id_t        current_screen = SCREEN_MAIN;
static bool               initialized = false;

/* ---- Screen Interfaces ------------------------------------------------ */

#include "ui/ui_screens/screen_main.h"
#include "ui/ui_screens/screen_recovery.h"
#include "ui/ui_screens/screen_progress.h"
#include "ui/ui_screens/screen_notify.h"

/* ---- Event Handler ---------------------------------------------------- */

static void on_screen_change(const event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev && ev->int_param >= 0 && ev->int_param < SCREEN_COUNT) {
        ui_manager_navigate((screen_id_t)ev->int_param);
    }
}

/* ---- Public API ------------------------------------------------------- */

bool ui_manager_init(void)
{
    memset(screens, 0, sizeof(screens));
    memset(screen_registered, 0, sizeof(screen_registered));

    /* Register built-in screens */
    ui_manager_register_screen(SCREEN_MAIN,     screen_main_get_interface());
    ui_manager_register_screen(SCREEN_RECOVERY, screen_recovery_get_interface());
    ui_manager_register_screen(SCREEN_PROGRESS, screen_progress_get_interface());
    ui_manager_register_screen(SCREEN_NOTIFY,   screen_notify_get_interface());

    /* Subscribe to screen navigation events */
    event_bus_subscribe(EVENT_SCREEN_CHANGE, on_screen_change, NULL);

    /* Load the main screen */
    ui_manager_navigate(SCREEN_MAIN);

    initialized = true;
    printf("ui: manager initialized\n");
    return true;
}

void ui_manager_deinit(void)
{
    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (screen_registered[i] && screens[i].destroy) {
            screens[i].destroy();
        }
    }
    memset(screen_registered, 0, sizeof(screen_registered));
    initialized = false;
}

void ui_manager_register_screen(screen_id_t id, const screen_interface_t *iface)
{
    if (id >= SCREEN_COUNT || iface == NULL) return;
    screens[id] = *iface;
    screen_registered[id] = true;
    printf("ui: registered screen %d\n", id);
}

void ui_manager_navigate(screen_id_t id)
{
    if (id >= SCREEN_COUNT || !screen_registered[id]) {
        fprintf(stderr, "ui: invalid screen %d\n", id);
        return;
    }

    screen_interface_t *target = &screens[id];
    screen_interface_t *prev   = screen_registered[current_screen]
                                   ? &screens[current_screen] : NULL;

    /* Hide previous screen */
    if (prev && prev->hide) prev->hide();

    /* Create target if needed */
    if (target->create) {
        target->create();
    }

    /* Show target */
    if (target->show) target->show();

    /* Destroy previous if not persistent */
    if (prev && !prev->keep_in_memory && prev->destroy) {
        prev->destroy();
    }

    current_screen = id;
    printf("ui: navigated to screen %d\n", id);
}

screen_id_t ui_manager_current_screen(void)
{
    return current_screen;
}

uint32_t ui_manager_tick(void)
{
    /* Apply deferred LVGL updates from worker thread before processing */
    screen_progress_apply_updates();

    uint32_t delay = lv_task_handler();
    /* Update time display if needed */
    static uint32_t last_time = 0;
    uint32_t now = utils_tick_get();
    if (now - last_time > 1000) {
        last_time = now;
        /* Time label update is handled by screen_show on each screen */
    }
    return delay;
}

void ui_manager_show_message(const char *title, const char *message, bool is_error)
{
    printf("ui: [%s] %s\n", title ? title : "INFO", message ? message : "");
}

boot_mode_t ui_manager_detect_boot_mode(void)
{
    FILE *fp = fopen("/proc/cmdline", "r");
    if (fp) {
        char cmdline[1024] = "";
        if (fgets(cmdline, sizeof(cmdline), fp)) {
            fclose(fp);
            if (strstr(cmdline, "bootmode=udisk") != NULL) {
                return BOOT_MODE_INSTALL;
            }
        } else {
            fclose(fp);
        }
    }
    return BOOT_MODE_RECOVERY;
}
