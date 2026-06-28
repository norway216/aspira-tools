#include "health_score.h"
#include <string.h>

/* ============================================================
 *  Clamp helper
 * ============================================================ */

static inline int clamp(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

/* ============================================================
 *  Health scoring formula
 *
 *  Base: score = 100
 *
 *  Penalties:
 *    reallocated sectors:    5 points each (max 40)
 *    pending sectors:       10 points each (max 30)
 *    uncorrectable errors:  20 points each (max 40)
 *    CRC errors:             1 point per 10 errors (max 10)
 *    temperature > 55°C:    10 points
 *    temperature > 65°C:    +1 per degree > 65 (max 30 total)
 *    nvme percentage_used > 80%: 1 point per % > 80 (max 20)
 *
 *  Score clamped to [0, 100].
 *
 *  Health state:
 *    >= 90: HEALTHY
 *    70–89: WARNING
 *    40–69: DEGRADED
 *     < 40: CRITICAL
 * ============================================================ */

void health_score_compute(const smart_ata_data_t  *ata,
                          const smart_nvme_data_t *nvme,
                          const io_metrics_t     *io,
                          health_score_t         *score) {
    memset(score, 0, sizeof(*score));

    int penalty = 0;

    /* --- ATA SMART penalties --- */
    if (ata && ata->supported) {
        /* Reallocated sectors */
        int realloc = (int)ata->reallocated_sectors;
        int realloc_p = clamp(5 * realloc, 0, 40);
        penalty += realloc_p;
        score->realloc_penalty = realloc_p;

        /* Pending sectors */
        int pending = (int)ata->pending_sectors;
        int pending_p = clamp(10 * pending, 0, 30);
        penalty += pending_p;
        score->pending_penalty = pending_p;

        /* Uncorrectable errors */
        int uncorr = (int)ata->uncorrectable_errors;
        int uncorr_p = clamp(20 * uncorr, 0, 40);
        penalty += uncorr_p;
        score->uncorrectable_penalty = uncorr_p;

        /* CRC errors */
        int crc = (int)ata->crc_errors;
        int crc_p = clamp(crc / 10, 0, 10);
        penalty += crc_p;
        score->crc_penalty = crc_p;

        /* Temperature */
        int temp = ata->temperature_celsius;
        int temp_p = 0;
        if (temp > 65) {
            temp_p = clamp(10 + (temp - 65), 0, 30);
        } else if (temp > 55) {
            temp_p = 10;
        }
        penalty += temp_p;
        score->temp_penalty = temp_p;
    }

    /* --- NVMe penalties --- */
    if (nvme && nvme->present) {
        int used = nvme->percentage_used;
        int nvme_p = 0;
        if (used > 80) {
            nvme_p = clamp(used - 80, 0, 20);
        }
        penalty += nvme_p;
        score->nvme_wear_penalty = nvme_p;
    }

    /* IO metrics are informational only in v1 (future: latency-based penalties) */
    (void)io;

    /* Final score */
    score->score = clamp(100 - penalty, 0, 100);

    /* Health state */
    if (score->score >= 90) {
        score->state = HEALTH_HEALTHY;
    } else if (score->score >= 70) {
        score->state = HEALTH_WARNING;
    } else if (score->score >= 40) {
        score->state = HEALTH_DEGRADED;
    } else {
        score->state = HEALTH_CRITICAL;
    }
    score->state_label = health_state_label(score->state);
}
