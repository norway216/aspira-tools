#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEFAULT_LOG_PATH      "/var/log/disk_health/disk_health.log"
#define DEFAULT_LOG_MAX_SIZE  (10 * 1024 * 1024)  /* 10 MiB */
#define DEFAULT_LOG_KEEP      5

/* ============================================================
 *  Initialize configuration with defaults
 * ============================================================ */

void config_init(config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->interval_seconds = DEFAULT_INTERVAL;
    cfg->log_level        = LOG_INFO;
    strncpy(cfg->log_path, DEFAULT_LOG_PATH, LOG_PATH_MAX - 1);
    cfg->log_path[LOG_PATH_MAX - 1] = '\0';
    cfg->watch_device[0]  = '\0';
    cfg->output_json      = 0;
    cfg->watch_mode       = 0;
    cfg->daemon_mode      = 0;
    cfg->log_max_size     = DEFAULT_LOG_MAX_SIZE;
    cfg->log_rotate_count = DEFAULT_LOG_KEEP;
    cfg->auto_heal        = 0;
    cfg->heal_max_level   = 2;
}

/* ============================================================
 *  Print usage
 * ============================================================ */

static void print_help(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\n"
           "Disk Health Monitoring Daemon\n\n"
           "Options:\n"
           "  --help              Show this help\n"
           "  --json              One-shot JSON output\n"
           "  --watch             Foreground continuous monitoring\n"
           "  --device PATH       Monitor a specific device\n"
           "  --interval SEC      Sampling interval (%d–%d, default %d)\n"
           "  --log-level LEVEL   Minimum log level (debug|info|warn|critical)\n"
           "  --log-path PATH     Log file path (default " DEFAULT_LOG_PATH ")\n"
           "  --log-max-size N    Max log size in MiB (0=no rotation, default 10)\n"
           "  --log-keep N        Number of backup log files (default 5)\n"
           "  --auto-heal         Enable automatic self-healing\n"
           "  --heal-max-level N  Maximum healing level (1–4, default 2)\n",
           MIN_INTERVAL, MAX_INTERVAL, DEFAULT_INTERVAL);
}

/* ============================================================
 *  Parse command-line arguments
 * ============================================================ */

int config_parse_args(config_t *cfg, int argc, char *argv[]) {
    const char *prog = argv[0];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help(prog);
            return 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            cfg->output_json = 1;
        } else if (strcmp(argv[i], "--watch") == 0) {
            cfg->watch_mode = 1;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            strncpy(cfg->watch_device, argv[++i], DEV_PATH_MAX - 1);
            cfg->watch_device[DEV_PATH_MAX - 1] = '\0';
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            int val = atoi(argv[++i]);
            if (val < MIN_INTERVAL) val = MIN_INTERVAL;
            if (val > MAX_INTERVAL) val = MAX_INTERVAL;
            cfg->interval_seconds = val;
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            const char *level = argv[++i];
            if (strcmp(level, "debug") == 0)       cfg->log_level = LOG_DEBUG;
            else if (strcmp(level, "info") == 0)    cfg->log_level = LOG_INFO;
            else if (strcmp(level, "warn") == 0)    cfg->log_level = LOG_WARN;
            else if (strcmp(level, "critical") == 0) cfg->log_level = LOG_CRITICAL;
            else {
                fprintf(stderr, "Unknown log level: %s\n", level);
                return 1;
            }
        } else if (strcmp(argv[i], "--log-path") == 0 && i + 1 < argc) {
            strncpy(cfg->log_path, argv[++i], LOG_PATH_MAX - 1);
            cfg->log_path[LOG_PATH_MAX - 1] = '\0';
        } else if (strcmp(argv[i], "--log-max-size") == 0 && i + 1 < argc) {
            int val = atoi(argv[++i]);
            cfg->log_max_size = (val < 0) ? 0 : (size_t)val * 1024 * 1024;
        } else if (strcmp(argv[i], "--log-keep") == 0 && i + 1 < argc) {
            int val = atoi(argv[++i]);
            if (val < 0) val = 0;
            if (val > 99) val = 99;
            cfg->log_rotate_count = val;
        } else if (strcmp(argv[i], "--auto-heal") == 0) {
            cfg->auto_heal = 1;
        } else if (strcmp(argv[i], "--heal-max-level") == 0 && i + 1 < argc) {
            int val = atoi(argv[++i]);
            if (val < 1) val = 1;
            if (val > 4) val = 4;
            cfg->heal_max_level = val;
        } else {
            fprintf(stderr, "Unknown option: %s (try --help)\n", argv[i]);
            return 1;
        }
    }

    return 0;
}

/* ============================================================
 *  Load configuration from file (INI-style)
 * ============================================================ */

int config_load_file(config_t *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        /* Skip empty lines and comments */
        if (len == 0 || line[0] == '#' || line[0] == ';') {
            continue;
        }

        char key[64] = {0};
        char value[192] = {0};

        if (sscanf(line, "%63[^=]=%191[^\n]", key, value) != 2) {
            continue;
        }

        /* Trim trailing whitespace from key */
        char *k = key + strlen(key) - 1;
        while (k >= key && (*k == ' ' || *k == '\t')) { *k = '\0'; k--; }

        /* Trim leading whitespace from value */
        char *v = value;
        while (*v == ' ' || *v == '\t') { v++; }

        if (strcmp(key, "interval") == 0) {
            int val = atoi(v);
            if (val < MIN_INTERVAL) val = MIN_INTERVAL;
            if (val > MAX_INTERVAL) val = MAX_INTERVAL;
            cfg->interval_seconds = val;
        } else if (strcmp(key, "log_path") == 0) {
            strncpy(cfg->log_path, v, LOG_PATH_MAX - 1);
            cfg->log_path[LOG_PATH_MAX - 1] = '\0';
        } else if (strcmp(key, "log_level") == 0) {
            if (strcmp(v, "debug") == 0)       cfg->log_level = LOG_DEBUG;
            else if (strcmp(v, "info") == 0)    cfg->log_level = LOG_INFO;
            else if (strcmp(v, "warn") == 0)    cfg->log_level = LOG_WARN;
            else if (strcmp(v, "critical") == 0) cfg->log_level = LOG_CRITICAL;
        } else if (strcmp(key, "log_max_size") == 0) {
            int val = atoi(v);
            cfg->log_max_size = (val < 0) ? 0 : (size_t)val * 1024 * 1024;
        } else if (strcmp(key, "log_keep") == 0) {
            int val = atoi(v);
            if (val < 0) val = 0;
            if (val > 99) val = 99;
            cfg->log_rotate_count = val;
        } else if (strcmp(key, "watch_device") == 0) {
            strncpy(cfg->watch_device, v, DEV_PATH_MAX - 1);
            cfg->watch_device[DEV_PATH_MAX - 1] = '\0';
        } else if (strcmp(key, "auto_heal") == 0) {
            cfg->auto_heal = (strcmp(v, "1") == 0 || strcmp(v, "true") == 0);
        } else if (strcmp(key, "heal_max_level") == 0) {
            int val = atoi(v);
            if (val < 1) val = 1;
            if (val > 4) val = 4;
            cfg->heal_max_level = val;
        }
    }

    fclose(f);
    return 0;
}
