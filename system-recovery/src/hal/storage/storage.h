#ifndef HAL_STORAGE_H
#define HAL_STORAGE_H

#include "common/types.h"
#include <stdbool.h>

/**
 * Storage HAL – abstracts partition mounting and block-device operations.
 */

int  storage_mount(mount_point_t *mp);
int  storage_umount(mount_point_t *mp);

/** Read version info from a recovery / install medium. */
int  storage_read_version(const char *path, version_info_t *info);

/** Check if a block device exists. */
bool storage_device_exists(const char *device);

/** Get the device path for external storage (USB vs SD card). */
const char *storage_get_external_device(void);

#endif /* HAL_STORAGE_H */
