#ifndef LOGGER_H
#define LOGGER_H

#include "disk_health.h"

int  logger_open(const char *path);
void logger_close(void);
void logger_write(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void logger_write_raw(const char *line);

/* Log rotation: set max file size (bytes) and backup count before opening.
   Call logger_set_rotate() after logger_open(). Rotation is checked
   automatically on each logger_write() call. */
void logger_set_rotate(size_t max_size, int keep_count);
int  logger_rotate(void);   /* force rotation; returns 1 if rotated */

/* Query current log file size */
size_t logger_current_size(void);

/* V2: Write a structured JSON healing event (per V2 Section 4.6) */
void logger_write_json_event(const char       *device,
                             fault_level_t     fault,
                             healing_action_t  action,
                             int               score,
                             int               failure_probability);

#endif
