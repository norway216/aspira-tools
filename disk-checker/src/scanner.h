#ifndef SCANNER_H
#define SCANNER_H

#include "disk_health.h"

int scan_block_devices(device_list_t *list);
int classify_device(const char *dev_path, device_info_t *info);
int get_device_info(const char *dev_path, device_info_t *info);

#endif
