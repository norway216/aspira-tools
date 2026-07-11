#ifndef SERVICES_LOG_SERVICE_H
#define SERVICES_LOG_SERVICE_H

#include "common/types.h"
#include <stdbool.h>

bool log_service_init(void);
void log_service_deinit(void);

void log_service_write(log_level_t level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

const char *log_service_get_path(void);

#endif /* SERVICES_LOG_SERVICE_H */
