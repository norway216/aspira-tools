/**
 * @file screen_recovery.c
 * @brief Recovery/Install operation selection screen.
 *
 * Shows radio-button style choices:
 *   - Lightweight Recovery / Deep Recovery
 *   - Light Install / Deep Install
 *   - App Recovery / App Backup
 *
 * Has Confirm / Cancel buttons, and launches the progress screen.
 */

#include "screen_recovery.h"
#include "ui/ui_manager.h"
#include "services/service_manager.h"
#include "core/event_bus.h"
#include "common/utils.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- State ------------------------------------------------------------ */

static lv_obj_t       *screen_obj = NULL;
static lv_obj_t       *time_label = NULL;
static lv_obj_t       *option_sw = NULL;
static lv_obj_t       *switch_label = NULL;
static operation_type_t selected_op = OPERATION_RECOVERY_LIGHT;
static boot_mode_t      boot_mode;
static bool             created = false;

/* Forward decls */
static void create(void);
static void show(void);
static void hide(void);
static void destroy(void);

static screen_interface_t g_iface = {
    .create  = create,
    .show    = show,
    .hide    = hide,
    .destroy = destroy,
    .keep_in_memory = false,
};

const screen_interface_t *screen_recovery_get_interface(void) { return &g_iface; }

/* ---- Helpers ---------------------------------------------------------- */

static void update_time(void)
{
    if (time_label == NULL) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d.%02d.%02d %02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    lv_label_set_text(time_label, buf);
}

/* ---- Callbacks -------------------------------------------------------- */

static void on_operation_complete(const operation_result_t *result, void *ctx)
{
    (void)ctx;
    printf("recovery: operation complete, success=%d\n", result->success);

    event_t ev = {
        .type = EVENT_OPERATION_COMPLETE,
        .int_param = result->success ? 1 : 0,
    };
    strncpy(ev.str_param, result->message, sizeof(ev.str_param) - 1);
    event_bus_publish(&ev);

    if (result->success) {
        ui_manager_navigate(SCREEN_NOTIFY);
    }
}

static void progress_handler(int percent, const char *status, void *ctx)
{
    (void)ctx;
    printf("recovery: progress %d%% – %s\n", percent, status);

    event_t ev = {
        .type = EVENT_OPERATION_PROGRESS,
        .int_param = percent,
    };
    strncpy(ev.str_param, status ? status : "", sizeof(ev.str_param) - 1);
    event_bus_publish(&ev);
}

static void on_confirm(lv_event_t *e)
{
    (void)e;
    printf("recovery: confirm – starting operation %d\n", selected_op);

    /* Navigate to progress screen first */
    ui_manager_navigate(SCREEN_PROGRESS);

    /* Start the operation */
    int ret = service_manager_start_operation(
        selected_op, progress_handler, on_operation_complete, NULL);

    if (ret != 0) {
        printf("recovery: failed to start operation\n");
        /* Show error notification */
        event_t ev = {
            .type = EVENT_OPERATION_COMPLETE,
            .int_param = 0,
            .str_param = "Failed to start operation",
        };
        event_bus_publish(&ev);
        ui_manager_navigate(SCREEN_NOTIFY);
    }
}

static void on_cancel(lv_event_t *e)
{
    (void)e;
    printf("recovery: cancelled, returning to main\n");
    ui_manager_navigate(SCREEN_MAIN);
}

static void on_switch_toggle(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (boot_mode == BOOT_MODE_RECOVERY) {
        /* Toggle between light and deep recovery */
        selected_op = checked ? OPERATION_RECOVERY_DEEP : OPERATION_RECOVERY_LIGHT;
        lv_label_set_text(switch_label, checked ? "Deep Recovery" : "Lightweight Recovery");
    } else {
        /* Toggle between light and deep install */
        selected_op = checked ? OPERATION_INSTALL_DEEP : OPERATION_INSTALL_LIGHT;
        lv_label_set_text(switch_label, checked ? "Deep Install" : "Lightweight Install");
    }
}

/* ---- Screen Interface ------------------------------------------------- */

static void create(void)
{
    if (created) return;

    boot_mode = ui_manager_detect_boot_mode();

    /* Default operation based on context */
    selected_op = (boot_mode == BOOT_MODE_RECOVERY)
                      ? OPERATION_RECOVERY_LIGHT
                      : OPERATION_INSTALL_LIGHT;

    screen_obj = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_obj, lv_color_make(176, 226, 255),
                              LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Title */
    lv_obj_t *title = lv_label_create(screen_obj);
    lv_label_set_text(title, (boot_mode == BOOT_MODE_RECOVERY)
                                 ? "Recovery Options" : "Installation Options");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_black(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* Operation mode switch */
    option_sw = lv_switch_create(screen_obj);
    lv_obj_set_size(option_sw, 80, 40);
    lv_obj_align(option_sw, LV_ALIGN_CENTER, 0, -30);
    lv_obj_add_event_cb(option_sw, on_switch_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    switch_label = lv_label_create(screen_obj);
    const char *sw_text = (boot_mode == BOOT_MODE_RECOVERY)
                              ? "Lightweight Recovery" : "Lightweight Install";
    lv_label_set_text(switch_label, sw_text);
    lv_obj_set_style_text_font(switch_label, &lv_font_montserrat_24, 0);
    lv_obj_align_to(switch_label, option_sw, LV_ALIGN_OUT_RIGHT_MID, 20, 0);

    /* Confirm button */
    lv_obj_t *btn_confirm = lv_btn_create(screen_obj);
    lv_obj_set_size(btn_confirm, 200, 60);
    lv_obj_align(btn_confirm, LV_ALIGN_BOTTOM_LEFT, 400, -50);
    lv_obj_set_style_bg_color(btn_confirm, lv_color_make(0, 150, 0), 0);

    lv_obj_t *confirm_label = lv_label_create(btn_confirm);
    lv_label_set_text(confirm_label, "Confirm");
    lv_obj_center(confirm_label);
    lv_obj_set_style_text_font(confirm_label, &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(btn_confirm, on_confirm, LV_EVENT_CLICKED, NULL);

    /* Cancel button */
    lv_obj_t *btn_cancel = lv_btn_create(screen_obj);
    lv_obj_set_size(btn_cancel, 200, 60);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -400, -50);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_make(150, 150, 150), 0);

    lv_obj_t *cancel_label = lv_label_create(btn_cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(btn_cancel, on_cancel, LV_EVENT_CLICKED, NULL);

    /* Time label */
    time_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(time_label, lv_color_black(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_LEFT, 1600, -30);

    created = true;
    printf("recovery: screen created\n");
}

static void show(void)
{
    if (screen_obj) {
        lv_scr_load_anim(screen_obj, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
        update_time();
    }
}

static void hide(void) { }

static void destroy(void)
{
    if (screen_obj) {
        lv_obj_del(screen_obj);
        screen_obj = NULL;
        time_label = NULL;
        option_sw = NULL;
        switch_label = NULL;
    }
    created = false;
}
