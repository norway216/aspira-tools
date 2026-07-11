#ifndef HAL_INPUT_MANAGER_H
#define HAL_INPUT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Input Manager -- unified interface for all input devices.
 *
 * Supports:
 *  - Touchpads (I2C HID, Synaptics, etc.) with relative motion
 *  - Touchscreens (absolute positioning)
 *  - Mice (relative motion with buttons)
 *  - Keyboards (standard HID keyboards)
 *  - Grape custom HID (proprietary key device)
 *
 * This module replaces the monolithic ext_input.c with a modular,
 * testable design where each device class has its own handler.
 */

#define INPUT_MAX_X  1920
#define INPUT_MAX_Y  1080

typedef struct {
    int32_t x;
    int32_t y;
    bool    pressed;       /**< Primary button / touch down */
    bool    right_pressed; /**< Secondary button (mice) */
} input_state_t;

/** Initialize input subsystem -- discover and open all devices. */
bool input_manager_init(void);

/** Poll all input devices and return the current unified state. */
void input_manager_poll(void);

/** Get the current combined cursor/button state. */
void input_manager_get_state(input_state_t *state);

/** Indicate that the LVGL indev has consumed a "pressed" event.
 *  Used for double-tap tracking. */
void input_manager_ack_press(void);

/** Get cursor position (for Grape key click-at-pointer). */
void input_manager_get_point(int32_t *x, int32_t *y);

/** Dump connected device information to stdout. */
void input_manager_dump_devices(void);

/** Gracefully close all device fds and clean up. */
void input_manager_deinit(void);

/** Return the build identifier string. */
const char *input_manager_build_id(void);

#endif /* HAL_INPUT_MANAGER_H */
