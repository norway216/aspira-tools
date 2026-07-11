/**
 * @file screen_notify.c
 * @brief Notification screen shown after operation completion.
 *
 * Displays success or failure message with an OK button,
 * then returns to the main screen.
 */

#include "screen_notify.h"
#include "core/event_bus.h"
#include "ui/ui_manager.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- State ------------------------------------------------------------ */

static lv_obj_t *screen_obj = NULL;
static lv_obj_t *msg_label = NULL;
static lv_obj_t *time_label = NULL;
static bool      created = false;
static bool      last_success = true;
static char      last_msg[256] = "";

/* ---- Event Handler ---------------------------------------------------- */

static void on_complete(const event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev == NULL) return;
    last_success = (ev->int_param != 0);
    strncpy(last_msg, ev->str_param, sizeof(last_msg) - 1);
}

/* ---- Button Handler --------------------------------------------------- */

static void on_ok(lv_event_t *e)
{
    (void)e;
    printf("notify: OK clicked, returning to main\n");
    ui_manager_navigate(SCREEN_MAIN);
}

/* ---- Screen Interface ------------------------------------------------- */

static void create(void)
{
    if (created) return;

    screen_obj = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_obj, lv_color_make(176, 226, 255),
                              LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Icon area */
    lv_obj_t *icon = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -50);

    /* Message label */
    msg_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(msg_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg_label, LV_ALIGN_CENTER, 0, 20);

    /* OK button */
    lv_obj_t *btn_ok = lv_btn_create(screen_obj);
    lv_obj_set_size(btn_ok, 200, 60);
    lv_obj_align(btn_ok, LV_ALIGN_CENTER, 0, 120);
    lv_obj_set_style_bg_color(btn_ok, lv_color_make(0, 120, 210), 0);

    lv_obj_t *ok_label = lv_label_create(btn_ok);
    lv_label_set_text(ok_label, "OK");
    lv_obj_center(ok_label);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_26, 0);
    lv_obj_add_event_cb(btn_ok, on_ok, LV_EVENT_CLICKED, NULL);

    /* Time label */
    time_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(time_label, lv_color_black(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_LEFT, 1600, -30);

    /* Subscribe to completion events (for update while visible) */
    event_bus_subscribe(EVENT_OPERATION_COMPLETE, on_complete, NULL);

    created = true;
    printf("notify: screen created\n");
}

static void show(void)
{
    if (screen_obj) {
        lv_scr_load_anim(screen_obj, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);

        /* Update labels based on last operation result */
        lv_obj_t *icon = lv_obj_get_child(screen_obj, 1);  /* icon is second child */
        if (icon) {
            lv_label_set_text(icon, last_success ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(icon,
                last_success ? lv_color_make(0, 170, 0) : lv_color_make(255, 0, 0), 0);
        }

        const char *title = last_success
            ? "Operation Completed Successfully!"
            : "Operation Failed!";
        lv_label_set_text(msg_label, title);

        /* Update time */
        if (time_label) {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char buf[32];
            snprintf(buf, sizeof(buf), "%04d.%02d.%02d %02d:%02d:%02d",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
            lv_label_set_text(time_label, buf);
        }
    }
}

static void hide(void) { }

static void destroy(void)
{
    if (screen_obj) {
        lv_obj_del(screen_obj);
        screen_obj = NULL;
        msg_label = NULL;
        time_label = NULL;
    }
    created = false;
}

static screen_interface_t g_iface = {
    .create  = create,
    .show    = show,
    .hide    = hide,
    .destroy = destroy,
    .keep_in_memory = false,
};

const screen_interface_t *screen_notify_get_interface(void) { return &g_iface; }
