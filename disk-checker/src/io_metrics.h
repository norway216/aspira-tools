#ifndef IO_METRICS_H
#define IO_METRICS_H

#include "disk_health.h"

int  io_metrics_read(const char *dev_name, io_metrics_t *metrics);
int  io_metrics_compute(io_metrics_t *cur, const io_metrics_t *prev, double interval_sec);
void io_metrics_copy(const io_metrics_t *src, io_metrics_t *dst);

#endif
