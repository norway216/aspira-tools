#include "smart_nvme.h"
#include "logger.h"

#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>

/* ============================================================
 *  Check if NVMe subsystem is present
 * ============================================================ */

int smart_nvme_check_present(const char *dev_path) {
    (void)dev_path;

    DIR *dir = opendir("/sys/class/nvme");
    if (!dir) {
        /* No NVMe subsystem — expected on many systems */
        return 0;
    }
    closedir(dir);
    return 1;
}

/* ============================================================
 *  Read NVMe SMART / Health Information
 *
 *  Reads from sysfs:
 *    /sys/class/nvme/<dev>/device/smart_log        (binary, 512 bytes)
 *    /sys/class/nvme/<dev>/device/device/...       (individual attributes)
 *
 *  NVMe SMART Log Page (Log Identifier 02h) layout:
 *    Byte 0:      Critical Warning
 *    Bytes 1–2:   Temperature (uint16 LE, Kelvin)
 *    Byte 3:      Percentage Used
 *    Bytes 4–15:  Reserved
 *    Bytes 16–31: Data Units Read   (uint128 LE, use lower 64 bits)
 *    Bytes 32–47: Data Units Written
 *    ...many more fields...
 *    Bytes 112–127: Media Errors (uint128 LE, use lower 64 bits)
 * ============================================================ */

int smart_nvme_read(const char *dev_path, smart_nvme_data_t *data) {
    if (!data) return -1;
    memset(data, 0, sizeof(*data));

    /* Extract NVMe device name from path, e.g., /dev/nvme0n1 -> nvme0 */
    const char *basename = strrchr(dev_path, '/');
    if (basename) basename++; else basename = dev_path;

    /* Get controller name: "nvme0n1" -> "nvme0" */
    char ctrl_name[DEV_NAME_MAX] = {0};
    {
        const char *p = basename;
        const char *n_pos = NULL;
        for (const char *q = p; *q; q++) {
            if (*q == 'n' && q > p) {
                n_pos = q;
                break;
            }
        }
        if (n_pos) {
            size_t len = (size_t)(n_pos - p);
            if (len >= DEV_NAME_MAX) len = DEV_NAME_MAX - 1;
            memcpy(ctrl_name, p, len);
        } else {
            snprintf(ctrl_name, DEV_NAME_MAX, "%s", p);
        }
    }

    /* Try the binary SMART log file */
    char smart_log_path[192];
    snprintf(smart_log_path, sizeof(smart_log_path),
             "/sys/class/nvme/%s/device/smart_log", ctrl_name);

    FILE *f = fopen(smart_log_path, "rb");
    if (f) {
        uint8_t log_buf[512];
        size_t nread = fread(log_buf, 1, sizeof(log_buf), f);
        fclose(f);

        if (nread >= 4) {
            data->present          = 1;
            data->critical_warning = log_buf[0];
            data->temperature_kelvin = (uint16_t)log_buf[1]
                                     | ((uint16_t)log_buf[2] << 8);
            data->percentage_used  = log_buf[3];
        }
        if (nread >= 48) {
            /* Data Units Read: bytes 16–31 (uint128 LE) */
            data->data_units_read = (uint64_t)log_buf[16]
                                  | ((uint64_t)log_buf[17] << 8)
                                  | ((uint64_t)log_buf[18] << 16)
                                  | ((uint64_t)log_buf[19] << 24)
                                  | ((uint64_t)log_buf[20] << 32)
                                  | ((uint64_t)log_buf[21] << 40)
                                  | ((uint64_t)log_buf[22] << 48)
                                  | ((uint64_t)log_buf[23] << 56);
            /* Data Units Written: bytes 32–47 */
            data->data_units_written = (uint64_t)log_buf[32]
                                     | ((uint64_t)log_buf[33] << 8)
                                     | ((uint64_t)log_buf[34] << 16)
                                     | ((uint64_t)log_buf[35] << 24)
                                     | ((uint64_t)log_buf[36] << 32)
                                     | ((uint64_t)log_buf[37] << 40)
                                     | ((uint64_t)log_buf[38] << 48)
                                     | ((uint64_t)log_buf[39] << 56);
        }
        if (nread >= 128) {
            /* Media Errors: bytes 112–127 */
            data->media_errors = (uint64_t)log_buf[112]
                               | ((uint64_t)log_buf[113] << 8)
                               | ((uint64_t)log_buf[114] << 16)
                               | ((uint64_t)log_buf[115] << 24)
                               | ((uint64_t)log_buf[116] << 32)
                               | ((uint64_t)log_buf[117] << 40)
                               | ((uint64_t)log_buf[118] << 48)
                               | ((uint64_t)log_buf[119] << 56);
        }
        logger_write(LOG_DEBUG, "smart_nvme: %s temp=%uK used=%u%% media_err=%lu",
                     dev_path, data->temperature_kelvin,
                     data->percentage_used,
                     (unsigned long)data->media_errors);
        return 0;
    }

    /* Fallback: try individual sysfs attribute files */
    char attr_path[256];

    data->present = 0;  /* will set to 1 if we find anything */

    snprintf(attr_path, sizeof(attr_path),
             "/sys/class/nvme/%s/device/critical_warning", ctrl_name);
    f = fopen(attr_path, "r");
    if (f) {
        unsigned int cw = 0;
        if (fscanf(f, "%u", &cw) == 1) {
            data->critical_warning = (uint8_t)cw;
            data->present = 1;
        }
        fclose(f);
    }

    snprintf(attr_path, sizeof(attr_path),
             "/sys/class/nvme/%s/device/temperature", ctrl_name);
    f = fopen(attr_path, "r");
    if (f) {
        unsigned int t = 0;
        if (fscanf(f, "%u", &t) == 1) {
            data->temperature_kelvin = (uint16_t)t;
            data->present = 1;
        }
        fclose(f);
    }

    snprintf(attr_path, sizeof(attr_path),
             "/sys/class/nvme/%s/device/media_errors", ctrl_name);
    f = fopen(attr_path, "r");
    if (f) {
        uint64_t me = 0;
        if (fscanf(f, "%lu", &me) == 1) {
            data->media_errors = me;
            data->present = 1;
        }
        fclose(f);
    }

    if (data->present) {
        logger_write(LOG_DEBUG, "smart_nvme: %s (sysfs) temp=%uK media_err=%lu",
                     dev_path, data->temperature_kelvin,
                     (unsigned long)data->media_errors);
    } else {
        logger_write(LOG_DEBUG, "smart_nvme: no NVMe health data for %s", dev_path);
    }

    return 0;
}
