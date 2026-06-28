#define _DEFAULT_SOURCE     /* for localtime_r */
#include "logger.h"
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>

/* ============================================================
 *  Internal state
 * ============================================================ */

static FILE   *log_file    = NULL;
static char    log_path_buf[LOG_PATH_MAX] = {0};
static size_t  log_current_size = 0;      /* bytes written to current file */
static size_t  log_max_size     = 0;      /* 0 = rotation disabled */
static int     log_keep_count   = 5;      /* number of backups to keep */
static int     log_in_rotate    = 0;      /* re-entry guard */

/* Default: 10 MiB max file size */
#define DEFAULT_LOG_MAX_SIZE    (10UL * 1024 * 1024)
#define DEFAULT_LOG_KEEP_COUNT  5
#define MAX_KEEP_COUNT          99

/* ============================================================
 *  Level → string
 * ============================================================ */

static const char *level_str(log_level_t level) {
    switch (level) {
        case LOG_DEBUG:    return "DEBUG";
        case LOG_INFO:     return "INFO";
        case LOG_WARN:     return "WARN";
        case LOG_CRITICAL: return "CRITICAL";
        default:           return "UNKNOWN";
    }
}

/* ============================================================
 *  Ensure parent directory exists (mkdir -p)
 * ============================================================ */

static int ensure_log_dir(const char *path) {
    char tmp[LOG_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = '\0';

    char *p = tmp;
    if (*p == '/') p++;

    while (*p) {
        char *next = strchr(p, '/');
        if (next) *next = '\0';

        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            /* non-critical */
        }

        if (next) {
            *next = '/';
            p = next + 1;
        } else {
            break;
        }
    }
    (void)mkdir(tmp, 0755);
    return 0;
}

/* ============================================================
 *  Rename helper: build backup path "base.N"
 * ============================================================ */

static void make_backup_path(char *dst, size_t dst_sz,
                              const char *base, int n) {
    snprintf(dst, dst_sz, "%s.%d", base, n);
}

/* ============================================================
 *  Open / close
 * ============================================================ */

int logger_open(const char *path) {
    if (log_file && log_file != stderr) {
        fclose(log_file);
    }
    log_file = NULL;
    log_current_size = 0;

    snprintf(log_path_buf, LOG_PATH_MAX, "%s", path);
    ensure_log_dir(path);

    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "[WARN] Cannot open log file '%s': %s, using stderr\n",
                path, strerror(errno));
        log_file = stderr;
        return -1;
    }

    setvbuf(f, NULL, _IOLBF, 0);

    /* Get current file size (for append continuation after restart) */
    fseek(f, 0, SEEK_END);
    log_current_size = (size_t)ftell(f);

    log_file = f;
    return 0;
}

void logger_close(void) {
    if (log_file && log_file != stderr) {
        fflush(log_file);
        fclose(log_file);
    }
    log_file = NULL;
    log_current_size = 0;
}

/* ============================================================
 *  Rotation configuration
 * ============================================================ */

void logger_set_rotate(size_t max_size, int keep_count) {
    log_max_size   = (max_size > 0) ? max_size : 0;
    log_keep_count = (keep_count > 0 && keep_count <= MAX_KEEP_COUNT)
                     ? keep_count : DEFAULT_LOG_KEEP_COUNT;
}

size_t logger_current_size(void) {
    return log_current_size;
}

/* ============================================================
 *  Rotate: close current, shift backups, open new
 *
 *  Backup chain (newest → oldest):
 *    base.log      → current (active)
 *    base.log.1    → newest backup
 *    base.log.2    → ...
 *    base.log.N    → oldest backup (deleted on rotation)
 * ============================================================ */

int logger_rotate(void) {
    if (!log_file || log_file == stderr) return 0;
    if (log_max_size == 0) return 0;          /* rotation disabled */
    if (log_current_size < log_max_size) return 0;
    if (log_in_rotate) return 0;              /* prevent re-entry */

    log_in_rotate = 1;

    /* Use raw write to avoid recursion through logger_write */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(log_file, "[%s] [DEBUG] Log rotation triggered (size=%zu, limit=%zu)\n",
            ts, log_current_size, log_max_size);
    fflush(log_file);

    /* Close current log */
    fflush(log_file);
    fclose(log_file);
    log_file = NULL;

    /* Remove the oldest backup: base.log.N */
    char old_path[LOG_PATH_MAX + 8];
    make_backup_path(old_path, sizeof(old_path),
                     log_path_buf, log_keep_count);
    unlink(old_path);

    /* Shift backups: base.log.(N-1) → base.log.N  ...  base.log.2 → base.log.3 */
    for (int i = log_keep_count - 1; i >= 1; i--) {
        char src[LOG_PATH_MAX + 8];
        char dst[LOG_PATH_MAX + 8];
        make_backup_path(src, sizeof(src), log_path_buf, i);
        make_backup_path(dst, sizeof(dst), log_path_buf, i + 1);
        rename(src, dst);  /* best-effort; ignore errors if src doesn't exist */
    }

    /* Rename current log → base.log.1 */
    char backup1[LOG_PATH_MAX + 8];
    make_backup_path(backup1, sizeof(backup1), log_path_buf, 1);
    rename(log_path_buf, backup1);

    /* Open new (empty) log file */
    FILE *f = fopen(log_path_buf, "a");
    if (!f) {
        /* If we can't create new log, fall back to stderr */
        fprintf(stderr, "[ERROR] Cannot create new log after rotation: %s\n",
                strerror(errno));
        log_file = stderr;
        log_current_size = 0;
        return -1;
    }

    setvbuf(f, NULL, _IOLBF, 0);
    log_file = f;
    log_current_size = 0;

    /* Write the summary with raw write (avoid re-entering rotation check) */
    now = time(NULL);
    localtime_r(&now, &tm_buf);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(log_file, "[%s] [INFO] Log rotated (%d backups kept)\n",
            ts, log_keep_count);
    fflush(log_file);

    log_in_rotate = 0;
    return 1;
}

/* ============================================================
 *  Write (with auto-rotation check)
 * ============================================================ */

void logger_write(log_level_t level, const char *fmt, ...) {
    /* Check rotation before writing (skip if already inside a rotation) */
    if (!log_in_rotate && log_file && log_file != stderr
        && log_max_size > 0 && log_current_size >= log_max_size) {
        logger_rotate();
    }

    if (!log_file) {
        log_file = stderr;
    }

    /* Build timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    /* Format the log line into a temporary buffer so we can measure its size */
    char line_buf[LOG_MSG_MAX * 2];
    int prefix_len = snprintf(line_buf, sizeof(line_buf),
                              "[%s] [%s] ", ts, level_str(level));

    va_list ap;
    va_start(ap, fmt);
    int msg_len = vsnprintf(line_buf + prefix_len,
                            sizeof(line_buf) - (size_t)prefix_len - 2,
                            fmt, ap);
    va_end(ap);

    /* Append newline */
    size_t total = (size_t)(prefix_len + msg_len);
    if (total < sizeof(line_buf) - 1) {
        line_buf[total] = '\n';
        line_buf[total + 1] = '\0';
        total++;
    }

    /* Write to file */
    size_t written = fwrite(line_buf, 1, total, log_file);
    if (log_file != stderr) {
        log_current_size += written;
    }
    fflush(log_file);
}

void logger_write_raw(const char *line) {
    if (!log_file) {
        log_file = stderr;
    }
    size_t len = strlen(line);
    size_t written = fwrite(line, 1, len, log_file);
    if (log_file != stderr) {
        log_current_size += written;
    }
    fflush(log_file);
}

/* ============================================================
 *  V2: Structured JSON event log (per V2 Section 4.6)
 * ============================================================ */

void logger_write_json_event(const char       *device,
                             fault_level_t     fault,
                             healing_action_t  action,
                             int               score,
                             int               failure_probability) {
    /* Build ISO 8601 timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    logger_write(LOG_INFO,
                 "{\"device\":\"%s\",\"event\":\"%s\",\"action\":\"%s\","
                 "\"score\":%d,\"failure_probability\":%d,"
                 "\"timestamp\":\"%s\"}",
                 device,
                 fault_level_label(fault),
                 healing_action_label(action),
                 score,
                 failure_probability,
                 ts);
}
