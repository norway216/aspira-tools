#include "log_service.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *log_fp = NULL;
static char  log_path[256] = "/tmp/system-recovery.log";

bool log_service_init(void)
{
    log_fp = fopen(log_path, "a");
    if (log_fp == NULL) {
        fprintf(stderr, "log: cannot open %s\n", log_path);
        return false;
    }
    fprintf(log_fp, "=== System Recovery Log Started ===\n");
    fflush(log_fp);
    return true;
}

void log_service_deinit(void)
{
    if (log_fp) {
        fprintf(log_fp, "=== System Recovery Log Ended ===\n");
        fclose(log_fp);
        log_fp = NULL;
    }
}

void log_service_write(log_level_t level, const char *format, ...)
{
    static const char *level_str[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    const char *ls = (level >= 0 && level <= LOG_LEVEL_ERROR) ? level_str[level] : "???";

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    va_list args;
    va_start(args, format);

    if (log_fp) {
        fprintf(log_fp, "[%s] [%s] ", ts, ls);
        vfprintf(log_fp, format, args);
        fprintf(log_fp, "\n");
        fflush(log_fp);
    }

    /* Also echo to stderr for levels >= WARN */
    if (level >= LOG_LEVEL_WARN) {
        fprintf(stderr, "[%s] [%s] ", ts, ls);
        va_list args2;
        va_copy(args2, args);
        /* Note: va_start already consumed; we need a fresh start.
         * Since we can't easily re-va_start, we echo format only. */
        fprintf(stderr, "%s\n", format);
        va_end(args2);
    }

    va_end(args);
}

const char *log_service_get_path(void)
{
    return log_path;
}
