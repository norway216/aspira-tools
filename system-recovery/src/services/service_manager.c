#include "service_manager.h"
#include "services/operations/op_interface.h"
#include "services/log_service.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- Mapping: operation_type_t → plugin name -------------------------- */

static const char *plugin_for_type(operation_type_t type)
{
    switch (type) {
    case OPERATION_RECOVERY_LIGHT: return "light_recovery";
    case OPERATION_RECOVERY_DEEP:  return "deep_recovery";
    case OPERATION_RECOVERY_APP:   return "app_recovery";
    case OPERATION_INSTALL_LIGHT:  return "light_install";
    case OPERATION_INSTALL_DEEP:   return "deep_install";
    case OPERATION_BACKUP_APP:     return "app_backup";
    default: return NULL;
    }
}

/* ---- Worker Thread Context -------------------------------------------- */

#define SHUTDOWN_TIMEOUT_SEC 10   /* Max wait for worker thread on shutdown */

typedef struct {
    const operation_plugin_t *plugin;
    progress_callback_t       progress_cb;
    void (*complete_cb)(const operation_result_t *result, void *ctx);
    void                     *ctx;
    operation_result_t         result;
} worker_ctx_t;

static pthread_t     worker_thread;
static bool          worker_running = false;
static bool          cancel_requested = false;

static void *worker_thread_func(void *arg)
{
    worker_ctx_t *wctx = (worker_ctx_t *)arg;

    operation_result_t result;
    memset(&result, 0, sizeof(result));
    result.error_code = -1;

    /* If cancelled before execution starts, skip */
    if (!cancel_requested) {
        result = wctx->plugin->execute(wctx->progress_cb, wctx->ctx);
    } else {
        snprintf(result.message, sizeof(result.message), "Operation cancelled");
    }

    if (wctx->complete_cb && !cancel_requested) {
        wctx->complete_cb(&result, wctx->ctx);
    }

    worker_running = false;
    free(wctx);
    return NULL;
}

/* ---- Public API ------------------------------------------------------- */

bool service_manager_init(void)
{
    /* Operation plugins are auto-registered via constructor attributes.
     * No manual registration needed. */
    log_service_write(LOG_LEVEL_INFO, "Service manager initialized");
    return true;
}

void service_manager_deinit(void)
{
    if (worker_running) {
        cancel_requested = true;

        /* Wait for worker thread with timeout — don't block shutdown forever */
        for (int i = 0; i < SHUTDOWN_TIMEOUT_SEC * 10; i++) {
            if (!worker_running) break;
            usleep(100000);  /* 100 ms */
        }

        if (worker_running) {
            log_service_write(LOG_LEVEL_ERROR,
                "Worker thread did not finish within %d s — detaching",
                SHUTDOWN_TIMEOUT_SEC);
            pthread_detach(worker_thread);
            worker_running = false;
        }
    }
    operation_plugin_deregister_all();
}

int service_manager_start_operation(operation_type_t type,
                                    progress_callback_t progress_cb,
                                    void (*complete_cb)(const operation_result_t *result,
                                                        void *ctx),
                                    void *ctx)
{
    if (worker_running) {
        log_service_write(LOG_LEVEL_WARN, "Operation already in progress");
        return -1;
    }

    const char *plugin_name = plugin_for_type(type);
    if (plugin_name == NULL) {
        log_service_write(LOG_LEVEL_ERROR, "Unknown operation type %d", type);
        return -1;
    }

    const operation_plugin_t *plugin = operation_plugin_find(plugin_name);
    if (plugin == NULL) {
        log_service_write(LOG_LEVEL_ERROR, "Plugin '%s' not found", plugin_name);
        return -1;
    }

    /* Pre-flight validation */
    if (plugin->validate && plugin->validate() != 0) {
        log_service_write(LOG_LEVEL_ERROR, "Plugin '%s' validation failed", plugin_name);
        return -1;
    }

    /* Init */
    if (plugin->init && plugin->init() != 0) {
        log_service_write(LOG_LEVEL_ERROR, "Plugin '%s' init failed", plugin_name);
        return -1;
    }

    log_service_write(LOG_LEVEL_INFO, "Starting operation: %s (%s)",
                      plugin_name, plugin->description);

    worker_ctx_t *wctx = calloc(1, sizeof(worker_ctx_t));
    if (wctx == NULL) return -1;

    wctx->plugin      = plugin;
    wctx->progress_cb = progress_cb;
    wctx->complete_cb = complete_cb;
    wctx->ctx         = ctx;

    worker_running  = true;
    cancel_requested = false;

    if (pthread_create(&worker_thread, NULL, worker_thread_func, wctx) != 0) {
        log_service_write(LOG_LEVEL_ERROR, "Failed to create worker thread");
        free(wctx);
        worker_running = false;
        return -1;
    }

    return 0;
}

bool service_manager_is_busy(void)
{
    return worker_running;
}

void service_manager_cancel(void)
{
    cancel_requested = true;
}

bool service_manager_cancelled(void)
{
    return cancel_requested;
}
