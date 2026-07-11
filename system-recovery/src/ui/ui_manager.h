#ifndef UI_UI_MANAGER_H
#define UI_UI_MANAGER_H

#include "common/types.h"
#include <stdbool.h>

/**
 * UI Manager – screen lifecycle and navigation.
 *
 * Each screen registers a create/show/hide/destroy interface.
 * The manager handles transitions (fade animation) and
 * forwards LVGL ticks.
 */

/** Interface that every screen must implement. */
typedef struct {
    /** Create all LVGL objects for this screen. Called once. */
    void (*create)(void);

    /** Called each time the screen becomes visible. */
    void (*show)(void);

    /** Called each time the screen is hidden. */
    void (*hide)(void);

    /** Destroy LVGL objects and free resources. */
    void (*destroy)(void);

    /** Whether this screen should persist in memory when hidden. */
    bool keep_in_memory;
} screen_interface_t;

bool ui_manager_init(void);
void ui_manager_deinit(void);

/** Register a screen implementation. */
void ui_manager_register_screen(screen_id_t id, const screen_interface_t *iface);

/** Navigate to a screen. */
void ui_manager_navigate(screen_id_t id);

/** Get the currently active screen ID. */
screen_id_t ui_manager_current_screen(void);

/** Process one tick of LVGL tasks (~60 Hz). */
void ui_manager_tick(void);

/** Show a message dialog to the user. */
void ui_manager_show_message(const char *title, const char *message, bool is_error);

/** Detect boot mode by reading /proc/cmdline. */
boot_mode_t ui_manager_detect_boot_mode(void);

#endif /* UI_UI_MANAGER_H */
