#ifndef HEALTH_SCORE_H
#define HEALTH_SCORE_H

#include "disk_health.h"

void health_score_compute(const smart_ata_data_t  *ata,
                          const smart_nvme_data_t *nvme,
                          const io_metrics_t     *io,
                          health_score_t         *score);

#endif
