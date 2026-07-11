#ifndef SERVICES_SERVICE_MANAGER_H
#define SERVICES_SERVICE_MANAGER_H

#include "common/types.h"
#include <stdbool.h>

/**
 * Service Manager – central registry for business-logic services.
 *
 * Manages lifecycle of:
 *   - Recovery service
 *   - Install service
 *   - Backup service
 *
 * Also acts as a facade: the UI calls service_manager_start_operation()
 * which dispatches to the correct service based on operation_type_t.
 */

bool service_manager_init(void);
void service_manager_deinit(void);

/**
 * Start an asynchronous recovery/install/backup operation.
 *
 * @param type         The operation to run.
 * @param progress_cb  Callback for progress updates (called from worker thread).
 * @param complete_cb  Callback on completion (called from worker thread).
 * @param ctx          Opaque user pointer passed to callbacks.
 * @return 0 if the operation was started, -1 on error.
 */
int service_manager_start_operation(operation_type_t type,
                                    progress_callback_t progress_cb,
                                    void (*complete_cb)(const operation_result_t *result, void *ctx),
                                    void *ctx);

/** Check if an operation is currently in progress. */
bool service_manager_is_busy(void);

/** Cancel a running operation (best-effort). */
void service_manager_cancel(void);

#endif /* SERVICES_SERVICE_MANAGER_H */
