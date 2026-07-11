/**
 * @file screen_progress.c
 * @brief Progress screen shown during long-running operations.
 *
 * Receives progress events from the event bus (possibly from the worker
 * thread) but DEFERS all LVGL updates to the main thread via
 * screen_progress_apply_updates(), which is called from ui_manager_tick().
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

/* Subscriber handles — saved so we can unsubscribe on destroy */
static event_subscriber_t progress_handle = 0;
static event_subscriber_t complete_handle = 0;

/* Pending updates from worker thread — applied in main loop */
static volatile bool pending_progress = false;
static volatile int  pending_pct = 0;
static volatile bool pending_navigate = false;
static char          pending_status[128] = "";

/* ---- Event Handlers (may be called from ANY thread) ------------------- */

static void on_progress(const event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev == NULL) return;

    /* Store values — do NOT touch LVGL here (thread safety) */
    pending_pct = ev->int_param;
    if (ev->str_param[0]) {
        strncpy(pending_status, ev->str_param, sizeof(pending_status) - 1);
        pending_status[sizeof(pending_status) - 1] = '\0';
    }
    pending_progress = true;
}

static void on_complete(const event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev && ev->int_param == 1) {
        printf("progress: operation succeeded\n");
    } else {
        printf("progress: operation failed: %s\n", ev ? ev->str_param : "unknown");
    }
    /* Defer navigation to main thread */
    pending_navigate = true;
}

/* ---- Called from main thread to apply deferred LVGL updates ----------- */

void screen_progress_apply_updates(void)
{
    /* Apply progress bar / status updates */
    if (pending_progress) {
        pending_progress = false;
        if (bar) {
            lv_bar_set_value(bar, pending_pct, LV_ANIM_ON);
        }
        if (status_label && pending_status[0]) {
            lv_label_set_text(status_label, pending_status);
        }
    }

    /* Handle deferred navigation to notify screen */
    if (pending_navigate) {
        pending_navigate = false;
        ui_manager_navigate(SCREEN_NOTIFY);
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

    /* Subscribe — save handles for cleanup */
    progress_handle = event_bus_subscribe(EVENT_OPERATION_PROGRESS, on_progress, NULL);
    complete_handle = event_bus_subscribe(EVENT_OPERATION_COMPLETE, on_complete, NULL);

    pending_progress = false;
    pending_pct = 0;
    pending_navigate = false;
    pending_status[0] = '\0';

    created = true;
    printf("progress: screen created\n");
}

static void show(void)
{
    if (screen_obj) {
        lv_scr_load_anim(screen_obj, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
        if (bar) lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        if (status_label) lv_label_set_text(status_label, "Preparing...");
        pending_progress = false;
        pending_pct = 0;
        pending_status[0] = '\0';

        /* Update time */
        if (time_label) {
            time_t now = time(NULL);
            struct tm tm_buf;
            localtime_r(&now, &tm_buf);
            char buf[32];
            snprintf(buf, sizeof(buf), "%04d.%02d.%02d %02d:%02d:%02d",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
            lv_label_set_text(time_label, buf);
        }
    }
}

static void hide(void) { }

static void destroy(void)
{
    /* Unsubscribe BEFORE destroying LVGL objects */
    if (progress_handle) {
        event_bus_unsubscribe(progress_handle);
        progress_handle = 0;
    }
    if (complete_handle) {
        event_bus_unsubscribe(complete_handle);
        complete_handle = 0;
    }

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
