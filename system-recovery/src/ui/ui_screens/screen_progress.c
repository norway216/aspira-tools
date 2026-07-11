/**
 * @file screen_progress.c
 * @brief Progress screen shown during long-running operations.
 *
 * Displays a progress bar, status text, and a time label.
 * Listens for EVENT_OPERATION_PROGRESS and EVENT_OPERATION_COMPLETE
 * to update the UI from the worker thread's callbacks.
 */

#include "screen_progress.h"
#include "core/event_bus.h"
#include "ui/ui_manager.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- State ------------------------------------------------------------ */

static lv_obj_t *screen_obj = NULL;
static lv_obj_t *bar = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *time_label = NULL;
static bool      created = false;
static int       last_pct = 0;
static char      last_status[128] = "";

/* ---- Event Handlers --------------------------------------------------- */

static void on_progress(const event_t *ev, void *ctx)
{
    (void)ctx;
    if (bar && ev) {
        last_pct = ev->int_param;
        lv_bar_set_value(bar, ev->int_param, LV_ANIM_ON);
    }
    if (status_label && ev && ev->str_param[0]) {
        strncpy(last_status, ev->str_param, sizeof(last_status) - 1);
        lv_label_set_text(status_label, ev->str_param);
    }
}

static void on_complete(const event_t *ev, void *ctx)
{
    (void)ctx;
    /* Navigate to notify screen after a short delay */
    if (ev && ev->int_param == 1) {
        printf("progress: operation succeeded\n");
    } else {
        printf("progress: operation failed: %s\n", ev ? ev->str_param : "unknown");
    }
}

/* ---- Screen Interface ------------------------------------------------- */

static void create(void)
{
    if (created) return;

    screen_obj = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_obj, lv_color_make(176, 226, 255),
                              LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Title */
    lv_obj_t *title = lv_label_create(screen_obj);
    lv_label_set_text(title, "Operation in Progress");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_black(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* Progress bar */
    bar = lv_bar_create(screen_obj);
    lv_obj_set_size(bar, 800, 40);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, -30);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    /* Status label */
    status_label = lv_label_create(screen_obj);
    lv_label_set_text(status_label, "Preparing...");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(status_label, lv_color_make(51, 51, 51), 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 30);

    /* "Please wait" message */
    lv_obj_t *wait_label = lv_label_create(screen_obj);
    lv_label_set_text(wait_label, "Please do not power off the device.");
    lv_obj_set_style_text_font(wait_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(wait_label, lv_color_make(255, 0, 0), 0);
    lv_obj_align(wait_label, LV_ALIGN_CENTER, 0, 80);

    /* Time label */
    time_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(time_label, lv_color_black(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_LEFT, 1600, -30);

    /* Subscribe to progress events */
    event_bus_subscribe(EVENT_OPERATION_PROGRESS, on_progress, NULL);
    event_bus_subscribe(EVENT_OPERATION_COMPLETE, on_complete, NULL);

    created = true;
    printf("progress: screen created\n");
}

static void show(void)
{
    if (screen_obj) {
        lv_scr_load_anim(screen_obj, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
        if (bar) lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        if (status_label) lv_label_set_text(status_label, "Preparing...");
        last_pct = 0;
        last_status[0] = '\0';

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
        bar = NULL;
        status_label = NULL;
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

const screen_interface_t *screen_progress_get_interface(void) { return &g_iface; }
