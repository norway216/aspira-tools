#ifndef FAILURE_PREDICT_H
#define FAILURE_PREDICT_H

#include "disk_health.h"

/* Initialize trend data to zero */
void trend_data_init(trend_data_t *trend);

/* Feed a new sample into the trend window */
void trend_data_update(trend_data_t *trend,
                       const smart_ata_data_t *ata,
                       const io_metrics_t    *io);

/* Compute failure probability from current trend data */
void failure_predict_compute(const trend_data_t *trend,
                             const health_score_t *score,
                             predict_result_t *result);

#endif
