#ifndef DISK_HEALTH_H
#define DISK_HEALTH_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* ============================================================
 *  Limits & Constants
 * ============================================================ */

#define MAX_DEVICES         32
#define DEV_PATH_MAX        64
#define DEV_NAME_MAX        32
#define DEV_MODEL_MAX       48
#define DEV_SERIAL_MAX      24
#define DEV_FW_REV_MAX      8
#define LOG_MSG_MAX         256
#define LOG_PATH_MAX        256
#define JSON_BUF_SIZE       4096
#define SMART_BUF_SIZE      512
#define MIN_INTERVAL         5
#define MAX_INTERVAL       120
#define DEFAULT_INTERVAL    30

/* ============================================================
 *  Device types
 * ============================================================ */

typedef enum {
    DEVICE_TYPE_UNKNOWN = 0,
    DEVICE_TYPE_SATA,
    DEVICE_TYPE_NVME
} device_type_t;

typedef struct {
    char           path[DEV_PATH_MAX];
    char           name[DEV_NAME_MAX];       /* basename, e.g. "sda" */
    char           model[DEV_MODEL_MAX];
    char           serial[DEV_SERIAL_MAX];
    char           fw_rev[DEV_FW_REV_MAX];
    device_type_t  type;
    int            rotational;               /* 1=HDD, 0=SSD */
} device_info_t;

typedef struct {
    device_info_t  devices[MAX_DEVICES];
    int            count;
} device_list_t;

/* ============================================================
 *  ATA SMART data
 * ============================================================ */

typedef struct {
    int       supported;            /* SMART feature set present */
    int       enabled;              /* SMART is enabled on drive */
    /* --- raw attribute values --- */
    uint64_t  reallocated_sectors;    /* ID   5 */
    uint64_t  pending_sectors;        /* ID 197 */
    uint64_t  uncorrectable_errors;   /* ID 187 */
    uint64_t  offline_uncorrectable;  /* ID 198 */
    uint64_t  crc_errors;             /* ID 199 */
    uint64_t  power_on_hours;         /* ID   9 */
    uint64_t  raw_read_error_rate;    /* ID   1 */
    int       temperature_celsius;    /* ID 194 (or 190) */
    /* --- normalized values (0–100, higher = better) --- */
    int       norm_reallocated;       /* ID   5 */
    int       norm_pending;           /* ID 197 */
    /* --- raw 512-byte snapshot --- */
    uint8_t   raw_data[SMART_BUF_SIZE];
} smart_ata_data_t;

/* ============================================================
 *  NVMe SMART data
 * ============================================================ */

typedef struct {
    int       present;              /* NVMe subsystem accessible */
    uint8_t   critical_warning;
    uint16_t  temperature_kelvin;
    uint8_t   percentage_used;
    uint64_t  data_units_read;
    uint64_t  data_units_written;
    uint64_t  media_errors;
} smart_nvme_data_t;

/* ============================================================
 *  I/O metrics
 * ============================================================ */

typedef struct {
    /* raw cumulative counters from /proc/diskstats */
    uint64_t  read_ios;
    uint64_t  read_sectors;
    uint64_t  read_ticks;
    uint64_t  write_ios;
    uint64_t  write_sectors;
    uint64_t  write_ticks;
    uint64_t  io_ticks;
    uint64_t  time_in_queue;
    /* computed deltas (per-second) */
    double    read_iops;
    double    write_iops;
    double    avg_latency_ms;
    int       valid;                /* 1 = deltas were computed */
} io_metrics_t;

/* ============================================================
 *  Health score
 * ============================================================ */

typedef enum {
    HEALTH_HEALTHY  = 0,
    HEALTH_WARNING  = 1,
    HEALTH_DEGRADED = 2,
    HEALTH_CRITICAL = 3
} health_state_t;

typedef struct {
    int            score;            /* 0–100 */
    health_state_t state;
    const char    *state_label;
    /* penalty breakdown */
    int            realloc_penalty;
    int            pending_penalty;
    int            uncorrectable_penalty;
    int            temp_penalty;
    int            crc_penalty;
    int            nvme_wear_penalty;
} health_score_t;

/* ============================================================
 *  V2: Fault Detection
 * ============================================================ */

typedef enum {
    FAULT_INFO     = 0,
    FAULT_WARN     = 1,
    FAULT_DEGRADED = 2,
    FAULT_CRITICAL = 3,
    FAULT_FATAL    = 4
} fault_level_t;

typedef struct {
    fault_level_t level;
    const char   *reason;
    int           pending_increasing;
    int           realloc_increasing;
    int           temp_spike;
    int           latency_spike;
} fault_result_t;

/* ============================================================
 *  V2: Policy Engine & Self-Healing
 * ============================================================ */

typedef enum {
    ACTION_NONE      = 0,
    ACTION_WARN      = 1,
    ACTION_THROTTLE  = 2,
    ACTION_REDUCE_IO = 3,
    ACTION_READONLY  = 4,
    ACTION_ISOLATE   = 5,
    ACTION_FAILOVER  = 6,
    ACTION_EMERGENCY = 7
} healing_action_t;

typedef enum {
    HEAL_LEVEL_SOFT       = 1,   /* WARN + THROTTLE */
    HEAL_LEVEL_CONTROLLED = 2,   /* + READONLY */
    HEAL_LEVEL_ISOLATION  = 3,   /* + ISOLATE + FAILOVER */
    HEAL_LEVEL_EMERGENCY  = 4    /* + EMERGENCY */
} healing_level_t;

typedef struct {
    healing_action_t action;
    healing_level_t  level;
    const char      *description;
    int              require_confirmation;
} policy_decision_t;

/* ============================================================
 *  V2: Failure Prediction (trend analysis)
 * ============================================================ */

#define TREND_WINDOW_SIZE 10

typedef struct {
    double   realloc_history[TREND_WINDOW_SIZE];
    double   pending_history[TREND_WINDOW_SIZE];
    double   temp_history[TREND_WINDOW_SIZE];
    double   latency_history[TREND_WINDOW_SIZE];
    int      history_pos;
    int      history_count;
    /* computed trends */
    double   realloc_slope;
    double   pending_slope;
    double   temp_slope;
    double   latency_mean;
    double   latency_stddev;
} trend_data_t;

typedef struct {
    int        failure_probability;    /* 0–100% */
    int        days_until_critical;    /* estimated, -1 = unknown */
    const char *primary_risk;
} predict_result_t;

/* ============================================================
 *  Logging
 * ============================================================ */

typedef enum {
    LOG_DEBUG    = 0,
    LOG_INFO     = 1,
    LOG_WARN     = 2,
    LOG_CRITICAL = 3
} log_level_t;

/* ============================================================
 *  Configuration
 * ============================================================ */

typedef struct {
    int          interval_seconds;
    log_level_t  log_level;
    char         log_path[LOG_PATH_MAX];
    char         watch_device[DEV_PATH_MAX];  /* "" = all devices */
    int          output_json;
    int          watch_mode;
    int          daemon_mode;
    /* log rotation */
    size_t       log_max_size;       /* max bytes before rotation, 0 = no rotation */
    int          log_rotate_count;   /* number of backup files to keep (1–99) */
    /* V2: self-healing */
    int          auto_heal;          /* 1 = execute healing, 0 = dry-run (default) */
    int          heal_max_level;     /* max healing level (1–4, default 2) */
} config_t;

/* ============================================================
 *  JSON builder
 * ============================================================ */

#define JSON_MAX_DEPTH 16

typedef struct {
    char  buf[JSON_BUF_SIZE];
    int   pos;
    int   depth;
    int   item_count[JSON_MAX_DEPTH];
} json_builder_t;

/* ============================================================
 *  Global daemon state
 * ============================================================ */

extern volatile int g_shutdown;
extern volatile int g_reload;
extern volatile int g_daemonized;

/* ============================================================
 *  Inline helpers
 * ============================================================ */

static inline const char *fault_level_label(fault_level_t f) {
    switch (f) {
        case FAULT_INFO:     return "INFO";
        case FAULT_WARN:     return "WARN";
        case FAULT_DEGRADED: return "DEGRADED";
        case FAULT_CRITICAL: return "CRITICAL";
        case FAULT_FATAL:    return "FATAL";
        default:             return "UNKNOWN";
    }
}

static inline const char *healing_action_label(healing_action_t a) {
    switch (a) {
        case ACTION_NONE:      return "NONE";
        case ACTION_WARN:      return "WARN";
        case ACTION_THROTTLE:  return "THROTTLE_IO";
        case ACTION_REDUCE_IO: return "REDUCE_IO";
        case ACTION_READONLY:  return "READONLY";
        case ACTION_ISOLATE:   return "ISOLATE";
        case ACTION_FAILOVER:  return "FAILOVER";
        case ACTION_EMERGENCY: return "EMERGENCY";
        default:               return "UNKNOWN";
    }
}

static inline const char *health_state_label(health_state_t s) {
    switch (s) {
        case HEALTH_HEALTHY:  return "HEALTHY";
        case HEALTH_WARNING:  return "WARNING";
        case HEALTH_DEGRADED: return "DEGRADED";
        case HEALTH_CRITICAL: return "CRITICAL";
        default:              return "UNKNOWN";
    }
}

/* string trim helper — strips trailing spaces and newlines */
static inline char *str_trim_tail(char *s) {
    char *end = s;
    while (*end) end++;
    while (end > s && (*(end - 1) == ' ' || *(end - 1) == '\n' || *(end - 1) == '\r'))
        *(--end) = '\0';
    return s;
}

#endif /* DISK_HEALTH_H */
