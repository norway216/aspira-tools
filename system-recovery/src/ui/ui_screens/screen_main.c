/**
 * @file screen_main.c
 * @brief Main menu screen – dual-mode (recovery / install) based on boot mode.
 *
 * Layout:
 *   [Title]
 *   [System Recovery / System Installation]
 *   [Software Recovery / Backup Software]
 *   [Reboot]    [Shutdown]
 *
 * All UI creation uses raw LVGL v8 API calls (same paradigm as legacy code).
 */

#include "screen_main.h"
#include "core/event_bus.h"
#include "services/service_manager.h"
#include "ui/ui_manager.h"
#include "common/utils.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* ---- Static State ----------------------------------------------------- */

static lv_obj_t    *screen_obj = NULL;
static lv_obj_t    *title_label = NULL;
static lv_obj_t    *time_label = NULL;
static lv_obj_t    *version_label = NULL;
static lv_obj_t    *btn_primary = NULL;    /* Recover/Install System */
static lv_obj_t    *btn_secondary = NULL;  /* Recover/Backup Software */
static lv_obj_t    *btn_reboot = NULL;
static lv_obj_t    *btn_shutdown = NULL;
static boot_mode_t  boot_mode;

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
    .keep_in_memory = true,
};

const screen_interface_t *screen_main_get_interface(void) { return &g_iface; }

/* ---- Helpers ---------------------------------------------------------- */

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                              int w, int h,
                              void (*cb)(lv_event_t *))
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_make(70, 70, 70),
                              LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(label, lv_color_make(255, 128, 0),
                                LV_PART_MAIN | LV_STATE_DEFAULT);

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

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

/* ---- Button Handlers -------------------------------------------------- */

static void on_btn_primary(lv_event_t *e)
{
    (void)e;
    printf("main: primary button clicked (boot_mode=%d)\n", boot_mode);
    ui_manager_navigate(SCREEN_RECOVERY);
}

static void on_btn_secondary(lv_event_t *e)
{
    (void)e;
    printf("main: secondary button clicked\n");
    /* Navigate to recovery screen with secondary context */
    ui_manager_navigate(SCREEN_RECOVERY);
}

static void on_btn_reboot(lv_event_t *e)
{
    (void)e;
    printf("main: reboot requested\n");
    utils_shell_exec("sync");
    utils_shell_exec("reboot -f");
}

static void on_btn_shutdown(lv_event_t *e)
{
    (void)e;
    printf("main: shutdown requested\n");
    utils_shell_exec("sync");
    utils_shell_exec("poweroff -f");
}

/* ---- Screen Interface ------------------------------------------------- */

static void create(void)
{
    if (screen_obj != NULL) return;

    boot_mode = ui_manager_detect_boot_mode();

    screen_obj = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_obj, lv_color_make(176, 226, 255),
                              LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Title */
    title_label = lv_label_create(screen_obj);
    lv_label_set_text(title_label,
                      (boot_mode == BOOT_MODE_RECOVERY)
                          ? "System Recovery" : "System Installation");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title_label, lv_color_black(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    int bw = 400, bh = 100;

    /* Row 1 */
    const char *prim_text = (boot_mode == BOOT_MODE_RECOVERY)
                                ? "Recover System" : "Install System";
    const char *sec_text  = (boot_mode == BOOT_MODE_RECOVERY)
                                ? "Recover Software" : "Backup Software";

    btn_primary   = make_button(screen_obj, prim_text, bw, bh, on_btn_primary);
    btn_secondary = make_button(screen_obj, sec_text,  bw, bh, on_btn_secondary);

    lv_obj_align(btn_primary,   LV_ALIGN_TOP_LEFT,  450, 200);
    lv_obj_align(btn_secondary, LV_ALIGN_TOP_RIGHT, -450, 200);

    /* Row 2 */
    btn_reboot   = make_button(screen_obj, "Reboot",   bw, bh, on_btn_reboot);
    btn_shutdown = make_button(screen_obj, "Shutdown", bw, bh, on_btn_shutdown);

    lv_obj_align(btn_reboot,   LV_ALIGN_TOP_LEFT,  450, 350);
    lv_obj_align(btn_shutdown, LV_ALIGN_TOP_RIGHT, -450, 350);

    /* Version label */
    version_label = lv_label_create(screen_obj);
    lv_label_set_text(version_label, "System Version: --");
    lv_obj_set_style_text_font(version_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(version_label, lv_color_black(), 0);
    lv_obj_align(version_label, LV_ALIGN_BOTTOM_LEFT, 30, -30);

    /* Time label */
    time_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(time_label, lv_color_black(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_LEFT, 1600, -30);

    printf("main: screen created (boot_mode=%d)\n", boot_mode);
}

static void show(void)
{
    if (screen_obj) {
        lv_scr_load_anim(screen_obj, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
        update_time();
    }
}

static void hide(void)
{
    /* Main screen persists in memory */
}

static void destroy(void)
{
    if (screen_obj) {
        lv_obj_del(screen_obj);
        screen_obj = NULL;
        title_label = NULL;
        time_label = NULL;
        version_label = NULL;
        btn_primary = NULL;
        btn_secondary = NULL;
        btn_reboot = NULL;
        btn_shutdown = NULL;
    }
}
