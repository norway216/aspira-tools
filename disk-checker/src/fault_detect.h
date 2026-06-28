#ifndef FAULT_DETECT_H
#define FAULT_DETECT_H

#include "disk_health.h"

/* Main fault detection: classify current device state */
void fault_detect(const health_score_t  *score,
                  const smart_ata_data_t *ata,
                  const smart_nvme_data_t *nvme,
                  const io_metrics_t    *io,
                  const trend_data_t    *trend,
                  fault_result_t        *result);

#endif
