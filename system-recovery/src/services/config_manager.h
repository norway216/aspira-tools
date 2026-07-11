#ifndef SERVICES_CONFIG_MANAGER_H
#define SERVICES_CONFIG_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Configuration manager – loads settings from file with sensible defaults.
 *
 * Priority: 1) Environment variables  2) Config file  3) Hard-coded defaults
 */

bool config_manager_init(void);
void config_manager_deinit(void);

/* ---- Accessors ---- */

const char *config_get_string(const char *section, const char *key,
                              const char *default_val);
int         config_get_int(const char *section, const char *key, int default_val);
bool        config_get_bool(const char *section, const char *key, bool default_val);

/* ---- Well-known keys ---- */

/* [display] */
#define CFG_DISPLAY_WIDTH         "display", "width"
#define CFG_DISPLAY_HEIGHT        "display", "height"

/* [input] */
#define CFG_TOUCHPAD_SENSITIVITY  "input", "touchpad_sensitivity"
#define CFG_DOUBLE_CLICK_MS       "input", "double_click_ms"
#define CFG_INPUT_DEBUG           "input", "debug"

/* [storage] */
#define CFG_RECOVERY_PARTITION    "storage", "recovery_partition"
#define CFG_DATA_PARTITION        "storage", "data_partition"
#define CFG_ROOT_A_PARTITION      "storage", "root_a_partition"
#define CFG_ROOT_B_PARTITION      "storage", "root_b_partition"
#define CFG_ROOT_A_MOUNT          "storage", "root_a_mount"
#define CFG_ROOT_B_MOUNT          "storage", "root_b_mount"
#define CFG_DATA_MOUNT            "storage", "data_mount"
#define CFG_RECOVERY_MOUNT        "storage", "recovery_mount"

/* [recovery] */
#define CFG_TIMEOUT_MINUTES       "recovery", "timeout_minutes"
#define CFG_VERIFY_CHECKSUM       "recovery", "verify_checksum"

/* [paths] */
#define CFG_UPDATE_SCRIPT         "paths", "update_script"

/* [boot] */
#define CFG_BOOT_MODE             "boot", "mode"

#endif /* SERVICES_CONFIG_MANAGER_H */
