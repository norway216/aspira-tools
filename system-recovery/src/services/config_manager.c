#include "config_manager.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE    512
#define MAX_ENTRIES 128

typedef struct {
    char section[64];
    char key[64];
    char value[256];
} config_entry_t;

static config_entry_t entries[MAX_ENTRIES];
static int            entry_count = 0;

static const char *config_paths[] = {
    "./config/default_config.ini",        /* Load defaults first */
    "/etc/system-recovery/config.ini",    /* Overlay runtime config */
    NULL
};

/* ---- Helpers ----------------------------------------------------------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static void set_entry(const char *section, const char *key, const char *value)
{
    /* Update existing entry if present */
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].section, section) == 0 &&
            strcmp(entries[i].key, key) == 0) {
            strncpy(entries[i].value, value, sizeof(entries[i].value) - 1);
            return;
        }
    }
    /* Add new entry */
    if (entry_count < MAX_ENTRIES) {
        strncpy(entries[entry_count].section, section, sizeof(entries[0].section) - 1);
        strncpy(entries[entry_count].key,     key,     sizeof(entries[0].key) - 1);
        strncpy(entries[entry_count].value,   value,   sizeof(entries[0].value) - 1);
        entry_count++;
    }
}

static bool parse_ini_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) return false;

    char line[MAX_LINE];
    char current_section[64] = "";

    while (fgets(line, sizeof(line), fp)) {
        char *t = trim(line);
        if (t[0] == '\0' || t[0] == '#' || t[0] == ';') continue;

        if (t[0] == '[') {
            /* Section header */
            char *end = strchr(t, ']');
            if (end) {
                *end = '\0';
                strncpy(current_section, t + 1, sizeof(current_section) - 1);
            }
        } else {
            /* key = value */
            char *eq = strchr(t, '=');
            if (eq && current_section[0]) {
                *eq = '\0';
                char *key   = trim(t);
                char *value = trim(eq + 1);
                set_entry(current_section, key, value);
            }
        }
    }

    fclose(fp);
    return true;
}

/* ---- Public API ------------------------------------------------------- */

bool config_manager_init(void)
{
    entry_count = 0;

    /* Load all config files in order — later files overlay earlier ones */
    for (int i = 0; config_paths[i] != NULL; i++) {
        if (parse_ini_file(config_paths[i])) {
            printf("config: loaded %s (%d entries)\n", config_paths[i], entry_count);
        }
    }

    /* Environment overrides */
    const char *env;
    if ((env = getenv("RECOVERY_TOUCHPAD_SENS")) != NULL)
        set_entry("input", "touchpad_sensitivity", env);
    if ((env = getenv("RECOVERY_TOUCHPAD_DOUBLE_MS")) != NULL)
        set_entry("input", "double_click_ms", env);
    if ((env = getenv("RECOVERY_INPUT_DEBUG")) != NULL)
        set_entry("input", "debug", env);
    if ((env = getenv("RECOVERY_PARTITION")) != NULL)
        set_entry("storage", "recovery_partition", env);
    if ((env = getenv("RECOVERY_BOOT_MODE")) != NULL)
        set_entry("boot", "mode", env);

    return true;
}

void config_manager_deinit(void)
{
    entry_count = 0;
}

/* ---- Accessors --------------------------------------------------------- */

const char *config_get_string(const char *section, const char *key,
                              const char *default_val)
{
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].section, section) == 0 &&
            strcmp(entries[i].key, key) == 0) {
            return entries[i].value;
        }
    }
    return default_val;
}

int config_get_int(const char *section, const char *key, int default_val)
{
    const char *v = config_get_string(section, key, NULL);
    if (v == NULL) return default_val;

    char *end = NULL;
    long val = strtol(v, &end, 10);
    if (end == v || *end != '\0') return default_val;
    return (int)val;
}

bool config_get_bool(const char *section, const char *key, bool default_val)
{
    const char *v = config_get_string(section, key, NULL);
    if (v == NULL) return default_val;
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
        strcmp(v, "yes") == 0 || strcmp(v, "on") == 0) return true;
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
        strcmp(v, "no") == 0 || strcmp(v, "off") == 0) return false;
    return default_val;
}
