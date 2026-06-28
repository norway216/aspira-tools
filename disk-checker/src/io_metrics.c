#include "io_metrics.h"
#include "logger.h"

#include <string.h>
#include <stdlib.h>

/* ============================================================
 *  Parse /proc/diskstats
 *
 *  Each line format (kernel 4.18+):
 *    major minor name rio rmerge rsect ruse wio wmerge wsect wuse
 *    running use aveq
 *
 *  Fields we care about (field index after name in scanf):
 *    field 3: read I/Os completed       (rio)
 *    field 4: read merges               (rmerge) — ignored
 *    field 5: read sectors              (rsect)
 *    field 6: read ticks (ms)           (ruse)
 *    field 7: write I/Os completed      (wio)
 *    field 8: write merges              (wmerge) — ignored
 *    field 9: write sectors             (wsect)
 *    field 10: write ticks (ms)         (wuse)
 *    field 11: I/Os currently in flight (running)
 *    field 12: IO ticks (ms)            (use)
 *    field 13: time in queue (ms)       (aveq)
 * ============================================================ */

int io_metrics_read(const char *dev_name, io_metrics_t *metrics) {
    if (!dev_name || !metrics) return -1;
    memset(metrics, 0, sizeof(*metrics));

    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) {
        logger_write(LOG_WARN, "io_metrics: cannot open /proc/diskstats");
        return -1;
    }

    char line[512];
    int found = 0;

    while (fgets(line, (int)sizeof(line), f)) {
        unsigned int major, minor;
        char name[DEV_NAME_MAX];

        /* Parse the line */
        int n = sscanf(line, "%u %u %31s %lu %*u %lu %lu %lu %*u %lu %lu %*u %lu %lu",
                       &major, &minor, name,
                       &metrics->read_ios,
                       &metrics->read_sectors,
                       &metrics->read_ticks,
                       &metrics->write_ios,
                       &metrics->write_sectors,
                       &metrics->write_ticks,
                       &metrics->io_ticks,
                       &metrics->time_in_queue);

        if (n >= 11 && strcmp(name, dev_name) == 0) {
            found = 1;
            break;
        }
    }

    fclose(f);

    if (!found) {
        logger_write(LOG_DEBUG, "io_metrics: device '%s' not found in /proc/diskstats",
                     dev_name);
        return -1;
    }

    return 0;
}

/* ============================================================
 *  Compute deltas between two samples
 * ============================================================ */

int io_metrics_compute(io_metrics_t *cur, const io_metrics_t *prev,
                        double interval_sec) {
    if (!cur || !prev || interval_sec <= 0.0) return -1;

    /* Check for counter wraparound */
    if (cur->read_ios  < prev->read_ios  ||
        cur->write_ios < prev->write_ios ||
        cur->read_ticks < prev->read_ticks ||
        cur->write_ticks < prev->write_ticks) {
        cur->valid = 0;
        return -1;  /* counter wrapped, skip this interval */
    }

    uint64_t delta_read_ios   = cur->read_ios   - prev->read_ios;
    uint64_t delta_write_ios  = cur->write_ios  - prev->write_ios;
    uint64_t delta_read_ticks  = cur->read_ticks  - prev->read_ticks;
    uint64_t delta_write_ticks = cur->write_ticks - prev->write_ticks;

    cur->read_iops  = (double)delta_read_ios  / interval_sec;
    cur->write_iops = (double)delta_write_ios / interval_sec;

    uint64_t total_ios = delta_read_ios + delta_write_ios;
    if (total_ios > 0) {
        cur->avg_latency_ms = (double)(delta_read_ticks + delta_write_ticks)
                            / (double)total_ios;
    } else {
        cur->avg_latency_ms = 0.0;
    }

    cur->valid = 1;
    return 0;
}

/* ============================================================
 *  Copy metrics (for prev snapshot update)
 * ============================================================ */

void io_metrics_copy(const io_metrics_t *src, io_metrics_t *dst) {
    if (src && dst) {
        memcpy(dst, src, sizeof(*dst));
    }
}
