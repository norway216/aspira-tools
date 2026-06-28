#include "failure_predict.h"
#include "health_score.h"
#include <string.h>
#include <math.h>

/* ============================================================
 *  Failure Prediction Engine (V2 Section 8)
 *
 *  Methods:
 *    - Moving average trend tracking (10-sample window)
 *    - Simple linear regression for slope detection
 *    - IO latency mean & stddev for anomaly detection
 *    - Failure probability score (0–100%)
 * ============================================================ */

/* ============================================================
 *  Initialize trend data
 * ============================================================ */

void trend_data_init(trend_data_t *trend) {
    memset(trend, 0, sizeof(*trend));
}

/* ============================================================
 *  Simple linear regression slope on a window of values
 *
 *  OLS: slope = Σ((x - x̄)(y - ȳ)) / Σ((x - x̄)²)
 *
 *  Returns slope (change per sample).
 * ============================================================ */

static double compute_slope(const double *values, int count) {
    if (count < 2) return 0.0;

    double sum_x = 0.0, sum_y = 0.0;
    double sum_xy = 0.0, sum_x2 = 0.0;

    for (int i = 0; i < count; i++) {
        sum_x  += (double)i;
        sum_y  += values[i];
        sum_xy += (double)i * values[i];
        sum_x2 += (double)i * (double)i;
    }

    double n = (double)count;
    double denom = n * sum_x2 - sum_x * sum_x;
    if (fabs(denom) < 1e-12) return 0.0;

    return (n * sum_xy - sum_x * sum_y) / denom;
}

/* ============================================================
 *  Compute mean of a window
 * ============================================================ */

static double compute_mean(const double *values, int count) {
    if (count == 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < count; i++) sum += values[i];
    return sum / (double)count;
}

/* ============================================================
 *  Compute standard deviation
 * ============================================================ */

static double compute_stddev(const double *values, int count, double mean) {
    if (count < 2) return 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < count; i++) {
        double diff = values[i] - mean;
        sum_sq += diff * diff;
    }
    return sqrt(sum_sq / (double)(count - 1));
}

/* ============================================================
 *  Feed a new sample into the circular trend window
 * ============================================================ */

void trend_data_update(trend_data_t *trend,
                       const smart_ata_data_t *ata,
                       const io_metrics_t    *io) {
    if (!trend) return;

    int pos = trend->history_pos;

    /* Store current values */
    if (ata && ata->supported) {
        trend->realloc_history[pos] = (double)ata->reallocated_sectors;
        trend->pending_history[pos] = (double)ata->pending_sectors;
        trend->temp_history[pos]    = (double)ata->temperature_celsius;
    } else {
        trend->realloc_history[pos] = 0.0;
        trend->pending_history[pos] = 0.0;
        trend->temp_history[pos]    = 0.0;
    }

    if (io && io->valid) {
        trend->latency_history[pos] = io->avg_latency_ms;
    } else {
        trend->latency_history[pos] = 0.0;
    }

    /* Advance circular buffer */
    trend->history_pos = (pos + 1) % TREND_WINDOW_SIZE;
    if (trend->history_count < TREND_WINDOW_SIZE) {
        trend->history_count++;
    }

    /* Recompute slopes */
    trend->realloc_slope = compute_slope(trend->realloc_history,
                                         trend->history_count);
    trend->pending_slope = compute_slope(trend->pending_history,
                                         trend->history_count);
    trend->temp_slope    = compute_slope(trend->temp_history,
                                         trend->history_count);

    /* Recompute latency statistics */
    trend->latency_mean = compute_mean(trend->latency_history,
                                       trend->history_count);
    trend->latency_stddev = compute_stddev(trend->latency_history,
                                           trend->history_count,
                                           trend->latency_mean);
}

/* ============================================================
 *  Compute failure probability
 *
 *  Probability increases with:
 *    - Higher reallocated/pending sector slopes
 *    - Lower current health score
 *    - Higher temperature
 *
 *  Returns 0–100%.
 * ============================================================ */

void failure_predict_compute(const trend_data_t *trend,
                             const health_score_t *score,
                             predict_result_t *result) {
    memset(result, 0, sizeof(*result));

    if (!trend || !score) {
        result->failure_probability = 0;
        result->days_until_critical = -1;
        result->primary_risk = "unknown";
        return;
    }

    /* Base probability from current score */
    double prob = (double)(100 - score->score);

    /* Add trend-based risk */
    if (trend->history_count >= 2) {
        /* Positive realloc slope adds risk */
        if (trend->realloc_slope > 0.0) {
            prob += trend->realloc_slope * 20.0;
        }
        /* Positive pending slope adds risk */
        if (trend->pending_slope > 0.0) {
            prob += trend->pending_slope * 30.0;
        }
        /* Rising temperature adds risk */
        if (trend->temp_slope > 0.5) {
            prob += trend->temp_slope * 5.0;
        }
    }

    /* Clamp to 0–100 */
    if (prob < 0.0) prob = 0.0;
    if (prob > 100.0) prob = 100.0;
    result->failure_probability = (int)(prob + 0.5);

    /* Determine primary risk factor */
    if (trend->pending_slope > trend->realloc_slope &&
        trend->pending_slope > 0.01) {
        result->primary_risk = "pending_sectors";
    } else if (trend->realloc_slope > 0.01) {
        result->primary_risk = "reallocated_sectors";
    } else if (trend->temp_slope > 0.5) {
        result->primary_risk = "temperature";
    } else if (score->score < 50) {
        result->primary_risk = "low_health_score";
    } else {
        result->primary_risk = "none";
    }

    /* Estimate days until critical (score < 40) */
    if (score->score <= 40) {
        result->days_until_critical = 0;
    } else if (trend->pending_slope > 0.01) {
        /* Each pending sector costs ~10 score points */
        double points_per_sample = trend->pending_slope * 10.0;
        int points_to_lose = score->score - 40;
        if (points_per_sample > 0.01) {
            /* Assume 30-second sampling → 2880 samples/day */
            double samples_needed = (double)points_to_lose / points_per_sample;
            result->days_until_critical = (int)(samples_needed / 2880.0);
        } else {
            result->days_until_critical = -1;
        }
    } else if (trend->realloc_slope > 0.01) {
        /* Each reallocated sector costs ~5 score points */
        double points_per_sample = trend->realloc_slope * 5.0;
        int points_to_lose = score->score - 40;
        if (points_per_sample > 0.01) {
            double samples_needed = (double)points_to_lose / points_per_sample;
            result->days_until_critical = (int)(samples_needed / 2880.0);
        } else {
            result->days_until_critical = -1;
        }
    } else {
        result->days_until_critical = -1;  /* cannot estimate */
    }

    if (result->days_until_critical < 0 && prob < 10.0) {
        result->days_until_critical = -1;
    }
}
