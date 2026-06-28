#ifndef SMART_ATA_H
#define SMART_ATA_H

#include "disk_health.h"

int  smart_ata_check_supported(const char *dev_path);
int  smart_ata_enable(const char *dev_path);
int  smart_ata_read(const char *dev_path, smart_ata_data_t *data);
void smart_ata_parse_raw(const uint8_t raw[SMART_BUF_SIZE], smart_ata_data_t *data);

#endif
