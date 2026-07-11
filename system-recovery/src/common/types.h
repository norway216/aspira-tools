/**
 * @file types.h
 * @brief Common type definitions for the System Recovery application.
 *
 * Centralizes all shared enums, structs, and type aliases to avoid
 * circular dependencies and ensure consistency across modules.
 */

#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/*  Operation Types                                                           */
/* -------------------------------------------------------------------------- */

/** Categories of recovery/install operations. */
typedef enum {
    OPERATION_RECOVERY_LIGHT = 0,   /**< Lightweight system recovery */
    OPERATION_RECOVERY_DEEP  = 1,   /**< Deep/full system recovery   */
    OPERATION_RECOVERY_APP   = 2,   /**< Application-level recovery  */
    OPERATION_INSTALL_LIGHT  = 3,   /**< Lightweight system install  */
    OPERATION_INSTALL_DEEP   = 4,   /**< Deep/full system install    */
    OPERATION_BACKUP_APP     = 5,   /**< Application backup          */
    OPERATION_COUNT                 /**< Sentinel / count             */
} operation_type_t;

/** Human-readable label for each operation type. */
const char *operation_type_name(operation_type_t type);

/* -------------------------------------------------------------------------- */
/*  Operation Result                                                          */
/* -------------------------------------------------------------------------- */

/** Outcome of a long-running operation. */
typedef struct {
    bool     success;       /**< true if completed without error */
    int      error_code;    /**< 0 on success, platform code otherwise */
    char     message[256];  /**< Human-readable status / error message */
} operation_result_t;

/** Signature for a progress callback.
 *  @param percent 0–100 completion
 *  @param status  Human-readable current step description
 *  @param ctx     Opaque user pointer
 */
typedef void (*progress_callback_t)(int percent, const char *status, void *ctx);

/* -------------------------------------------------------------------------- */
/*  Event System                                                              */
/* -------------------------------------------------------------------------- */

/** Well-known events flowing through the event bus. */
typedef enum {
    EVENT_NONE = 0,

    /* UI navigation */
    EVENT_SCREEN_CHANGE,
    EVENT_SCREEN_BACK,

    /* Operation lifecycle */
    EVENT_OPERATION_START,
    EVENT_OPERATION_PROGRESS,
    EVENT_OPERATION_COMPLETE,

    /* Input */
    EVENT_INPUT_KEY,
    EVENT_INPUT_POINTER,

    /* System */
    EVENT_SHUTDOWN,
    EVENT_REBOOT,
    EVENT_TIMEOUT,

    /* Application */
    EVENT_APP_QUIT,

    EVENT_COUNT
} event_type_t;

/** Payload carried by an event. */
typedef struct {
    event_type_t  type;
    int           int_param;
    void         *ptr_param;
    char          str_param[128];
} event_t;

/** Opaque handle for an event subscriber. */
typedef uint32_t event_subscriber_t;

/** Subscriber callback signature. */
typedef void (*event_handler_t)(const event_t *event, void *user_data);

/* -------------------------------------------------------------------------- */
/*  Screen Management                                                         */
/* -------------------------------------------------------------------------- */

/** All screens in the application. */
typedef enum {
    SCREEN_MAIN = 0,
    SCREEN_RECOVERY,
    SCREEN_INSTALL,
    SCREEN_BACKUP,
    SCREEN_PROGRESS,
    SCREEN_NOTIFY,
    SCREEN_COUNT
} screen_id_t;

/* -------------------------------------------------------------------------- */
/*  Boot Mode                                                                 */
/* -------------------------------------------------------------------------- */

typedef enum {
    BOOT_MODE_RECOVERY = 0,  /**< Normal recovery boot */
    BOOT_MODE_INSTALL  = 1,  /**< UDisk install boot   */
} boot_mode_t;

/* -------------------------------------------------------------------------- */
/*  Filesystem / Mount                                                        */
/* -------------------------------------------------------------------------- */

/** Describes a mount point used during recovery operations. */
typedef struct {
    const char *mount_path;   /**< Where to mount         */
    const char *device;       /**< Block device path      */
    const char *fs_type;      /**< Filesystem type (may be NULL for auto) */
    bool        is_mounted;   /**< Current mount state    */
    bool        dir_created;  /**< Was the directory created by us? */
} mount_point_t;

/* -------------------------------------------------------------------------- */
/*  Version Information                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    char model[64];
    char version[64];
} version_info_t;

/* -------------------------------------------------------------------------- */
/*  Logging                                                                   */
/* -------------------------------------------------------------------------- */

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
} log_level_t;

#endif /* COMMON_TYPES_H */
