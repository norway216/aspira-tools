#ifndef SMART_NVME_H
#define SMART_NVME_H

#include "disk_health.h"

int smart_nvme_read(const char *dev_path, smart_nvme_data_t *data);
int smart_nvme_check_present(const char *dev_path);

#endif
