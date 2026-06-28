#include "smart_ata.h"
#include "logger.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/hdreg.h>
#include <errno.h>

/* ============================================================
 *  SMART ATA constants
 * ============================================================ */

#define WIN_SMART          0xB0
#define SMART_READ_VALUES  0xD0
#define SMART_ENABLE       0xD8
#define SMART_STATUS       0xDA
#define SMART_LCYL_PASS    0x4F

/* ============================================================
 *  Check if SMART is supported
 * ============================================================ */

int smart_ata_check_supported(const char *dev_path) {
    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        logger_write(LOG_DEBUG, "smart_ata: cannot open %s: %s",
                     dev_path, strerror(errno));
        return 0;
    }

    struct hd_driveid id;
    if (ioctl(fd, HDIO_GET_IDENTITY, &id) < 0) {
        logger_write(LOG_DEBUG, "smart_ata: HDIO_GET_IDENTITY failed for %s: %s",
                     dev_path, strerror(errno));
        close(fd);
        return 0;
    }
    close(fd);

    /* command_set_1 (word 82) bit 0 = SMART Feature Set supported
     * command_set_2 (word 83) bit 0 = SMART self-test supported */
    int supported = (id.command_set_1 & 0x0001) ? 1 : 0;
    return supported;
}

/* ============================================================
 *  Enable SMART on the drive
 * ============================================================ */

int smart_ata_enable(const char *dev_path) {
    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        logger_write(LOG_DEBUG, "smart_ata: cannot open %s for enable: %s",
                     dev_path, strerror(errno));
        return -1;
    }

    uint8_t args[4] = {
        WIN_SMART,            /* command */
        SMART_LCYL_PASS,      /* sector_number = LBA Low */
        SMART_ENABLE,         /* feature = subcommand */
        1                     /* sector_count */
    };

    int ret = ioctl(fd, HDIO_DRIVE_CMD, args);
    close(fd);

    if (ret < 0) {
        /* EIO often means SMART is already enabled — not a real error */
        if (errno == EIO) {
            logger_write(LOG_DEBUG, "smart_ata: SMART already enabled on %s", dev_path);
            return 1;  /* already enabled */
        }
        logger_write(LOG_DEBUG, "smart_ata: SMART enable failed for %s: %s",
                     dev_path, strerror(errno));
        return -1;
    }

    return 0;
}

/* ============================================================
 *  Read SMART data (full 512-byte attribute sector)
 * ============================================================ */

int smart_ata_read(const char *dev_path, smart_ata_data_t *data) {
    if (!data) return -1;
    memset(data, 0, sizeof(*data));

    /* Check SMART support via IDENTITY */
    data->supported = smart_ata_check_supported(dev_path);
    if (!data->supported) {
        logger_write(LOG_DEBUG, "smart_ata: SMART not supported on %s", dev_path);
        return -1;
    }

    /* Enable SMART (ignore EIO = already enabled) */
    int en = smart_ata_enable(dev_path);
    if (en < 0) {
        logger_write(LOG_WARN, "smart_ata: cannot enable SMART on %s", dev_path);
        return -1;
    }
    data->enabled = 1;

    /* Read SMART attribute values */
    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        logger_write(LOG_WARN, "smart_ata: cannot open %s for read: %s",
                     dev_path, strerror(errno));
        return -1;
    }

    /* Build the command buffer: 4-byte header + 512-byte data */
    uint8_t buf[4 + SMART_BUF_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = WIN_SMART;            /* command */
    buf[1] = SMART_LCYL_PASS;      /* sector_number (LBA Low = 0x4F) */
    buf[2] = SMART_READ_VALUES;    /* feature = SMART subcommand */
    buf[3] = 1;                    /* sector_count = 1 sector */

    int ret = ioctl(fd, HDIO_DRIVE_CMD, buf);
    close(fd);

    if (ret < 0) {
        logger_write(LOG_WARN, "smart_ata: SMART READ DATA failed for %s: %s",
                     dev_path, strerror(errno));
        data->enabled = 0;
        return -1;
    }

    /* Copy the 512-byte SMART data area */
    memcpy(data->raw_data, buf + 4, SMART_BUF_SIZE);

    /* Parse into structured fields */
    smart_ata_parse_raw(data->raw_data, data);

    logger_write(LOG_DEBUG,
                 "smart_ata: %s temp=%d realloc=%lu pending=%lu uncorrect=%lu",
                 dev_path, data->temperature_celsius,
                 (unsigned long)data->reallocated_sectors,
                 (unsigned long)data->pending_sectors,
                 (unsigned long)data->uncorrectable_errors);

    return 0;
}

/* ============================================================
 *  Parse the 512-byte SMART attribute sector
 *
 *  Layout:
 *    Offset 0–1:   revision (uint16 LE)
 *    Offset 2–361: 30 attribute entries × 12 bytes each
 *      Byte 0:  attribute_id
 *      Byte 1:  flags
 *      Byte 2:  normalized current (0..100)
 *      Byte 3:  worst-ever
 *      Byte 4:  reserved
 *      Bytes 5–10: raw value (6 bytes, little-endian)
 *      Byte 11: reserved
 *    Offset 362+:  offline data collection status, checksum, etc.
 * ============================================================ */

void smart_ata_parse_raw(const uint8_t raw[SMART_BUF_SIZE], smart_ata_data_t *data) {
    if (!raw || !data) return;

    /* Iterate 30 attribute entries starting at offset 2 */
    for (int i = 0; i < 30; i++) {
        int off = 2 + i * 12;
        uint8_t attr_id = raw[off];

        if (attr_id == 0) continue;  /* unused slot */

        uint8_t  norm_val  = raw[off + 2];
        /* uint8_t  worst_val = raw[off + 3]; */
        uint64_t raw_val   = (uint64_t)raw[off + 5]
                           | ((uint64_t)raw[off + 6]  << 8)
                           | ((uint64_t)raw[off + 7]  << 16)
                           | ((uint64_t)raw[off + 8]  << 24)
                           | ((uint64_t)raw[off + 9]  << 32)
                           | ((uint64_t)raw[off + 10] << 40);

        switch (attr_id) {
        case 1:   /* Raw Read Error Rate */
            data->raw_read_error_rate = raw_val;
            break;
        case 5:   /* Reallocated Sector Count */
            data->reallocated_sectors = raw_val;
            data->norm_reallocated    = norm_val;
            break;
        case 9:   /* Power-On Hours */
            data->power_on_hours = raw_val;
            break;
        case 187: /* Reported Uncorrectable Errors */
            data->uncorrectable_errors = raw_val;
            break;
        case 190: /* Airflow Temperature (alternate sensor) */
            if (data->temperature_celsius == 0) {
                /* Some drives encode temp in raw byte 5, others use whole value */
                int t = (int)(raw_val & 0xFF);
                /* Sanity check: typical drive temps are 20–70°C */
                if (t >= 15 && t <= 80) {
                    data->temperature_celsius = t;
                }
            }
            break;
        case 194: /* Temperature Celsius (primary) */
            {
                int t = (int)(raw_val & 0xFF);
                if (t >= 15 && t <= 80) {
                    data->temperature_celsius = t;
                }
            }
            break;
        case 197: /* Current Pending Sector Count */
            data->pending_sectors  = raw_val;
            data->norm_pending     = norm_val;
            break;
        case 198: /* Offline Uncorrectable */
            data->offline_uncorrectable = raw_val;
            break;
        case 199: /* UltraDMA CRC Error Count */
            data->crc_errors = raw_val;
            break;
        default:
            break;
        }
    }
}
