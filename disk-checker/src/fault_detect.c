#include "fault_detect.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================
 *  Fault Detection Engine (V2 Section 4.3)
 *
 *  Classifies faults into 5 levels:
 *    FAULT_INFO     — normal behavior
 *    FAULT_WARN     — early degradation
 *    FAULT_DEGRADED — performance / health drop
 *    FAULT_CRITICAL — failure imminent
 *    FAULT_FATAL    — device unusable
 *
 *  Detection methods:
 *    1. Threshold-based: SMART attributes vs fixed limits
 *    2. Trend-based: current vs previous sample, direction
 *    3. IO anomaly: latency spike > 3x moving average
 * ============================================================ */

void fault_detect(const health_score_t  *score,
                  const smart_ata_data_t *ata,
                  const smart_nvme_data_t *nvme,
                  const io_metrics_t    *io,
                  const trend_data_t    *trend,
                  fault_result_t        *result) {
    memset(result, 0, sizeof(*result));
    result->level  = FAULT_INFO;
    result->reason = "Normal operation";

    /* --- Threshold-based detection --- */

    /* Fatal: score = 0 (device unusable) */
    if (score->score == 0) {
        result->level  = FAULT_FATAL;
        result->reason = "Device health score is zero — device unusable";
        return;
    }

    /* Critical: score < 40 */
    if (score->score < 40) {
        result->level  = FAULT_CRITICAL;
        result->reason = "Health score critically low — failure imminent";
        return;
    }

    /* Degraded: score < 70 */
    if (score->score < 70) {
        result->level = FAULT_DEGRADED;
        result->reason = "Health score indicates degradation";
        /* keep checking for additional signals */
    }

    /* --- SMART attribute threshold checks --- */

    if (ata && ata->supported) {
        /* Pending sectors > 0 is always a concern */
        if (ata->pending_sectors > 0) {
            if (result->level < FAULT_WARN) {
                result->level  = FAULT_WARN;
                result->reason = "Pending sectors detected";
            }
            if (ata->pending_sectors >= 5) {
                result->level  = FAULT_DEGRADED;
                result->reason = "High pending sector count";
            }
            if (ata->pending_sectors >= 50) {
                result->level  = FAULT_CRITICAL;
                result->reason = "Critical pending sector count";
                return;
            }
        }

        /* Reallocated sectors */
        if (ata->reallocated_sectors > 0) {
            if (result->level < FAULT_WARN) {
                result->level  = FAULT_WARN;
                result->reason = "Reallocated sectors present";
            }
            if (ata->reallocated_sectors >= 10) {
                result->level  = FAULT_DEGRADED;
                result->reason = "Many reallocated sectors";
            }
            if (ata->reallocated_sectors >= 100) {
                result->level  = FAULT_CRITICAL;
                result->reason = "Excessive reallocated sectors";
                return;
            }
        }

        /* Uncorrectable errors */
        if (ata->uncorrectable_errors > 0) {
            result->level  = FAULT_DEGRADED;
            result->reason = "Uncorrectable errors detected";
            if (ata->uncorrectable_errors >= 10) {
                result->level  = FAULT_CRITICAL;
                result->reason = "Many uncorrectable errors";
                return;
            }
        }

        /* Temperature check */
        if (ata->temperature_celsius > 60) {
            result->temp_spike = 1;
            if (result->level < FAULT_WARN) {
                result->level  = FAULT_WARN;
                result->reason = "High temperature";
            }
            if (ata->temperature_celsius > 70) {
                result->level  = FAULT_DEGRADED;
                result->reason = "Excessive temperature";
            }
        }
    }

    /* NVMe checks */
    if (nvme && nvme->present) {
        if (nvme->critical_warning != 0) {
            result->level  = FAULT_CRITICAL;
            result->reason = "NVMe critical warning active";
            return;
        }
        if (nvme->percentage_used >= 90) {
            result->level  = FAULT_DEGRADED;
            result->reason = "NVMe endurance nearly exhausted";
        }
        if (nvme->media_errors > 0) {
            result->level  = FAULT_DEGRADED;
            result->reason = "NVMe media errors detected";
        }
    }

    /* --- Trend-based detection --- */

    if (trend && trend->history_count >= 2) {
        /* Pending sectors increasing */
        if (trend->pending_slope > 0.01) {
            result->pending_increasing = 1;
            if (result->level < FAULT_WARN) {
                result->level  = FAULT_WARN;
                result->reason = "Pending sectors trending upward";
            }
            if (trend->pending_slope > 1.0) {
                result->level  = FAULT_DEGRADED;
                result->reason = "Pending sectors rapidly increasing";
            }
        }

        /* Reallocated sectors increasing */
        if (trend->realloc_slope > 0.01) {
            result->realloc_increasing = 1;
            if (result->level < FAULT_WARN) {
                result->level  = FAULT_WARN;
                result->reason = "Reallocated sectors trending upward";
            }
        }

        /* Temperature spike: increase > 10°C vs previous */
        if (trend->history_count >= 2) {
            int idx = trend->history_pos;
            int prev_idx = (idx - 2 + TREND_WINDOW_SIZE) % TREND_WINDOW_SIZE;
            double prev_temp = trend->temp_history[prev_idx];
            double cur_temp  = trend->temp_history[(idx - 1 + TREND_WINDOW_SIZE) % TREND_WINDOW_SIZE];
            if (cur_temp - prev_temp > 10.0) {
                result->temp_spike = 1;
                if (result->level < FAULT_WARN) {
                    result->level  = FAULT_WARN;
                    result->reason = "Temperature spike detected";
                }
            }
        }
    }

    /* --- IO anomaly detection --- */

    if (io && io->valid && trend && trend->history_count >= 3) {
        double threshold = trend->latency_mean * 3.0;
        if (threshold > 0.0 && io->avg_latency_ms > threshold) {
            result->latency_spike = 1;
            if (result->level < FAULT_WARN) {
                result->level  = FAULT_WARN;
                result->reason = "IO latency spike detected";
            }
            if (io->avg_latency_ms > threshold * 3.0) {
                result->level  = FAULT_DEGRADED;
                result->reason = "Severe IO latency anomaly";
            }
        }
    }
}
