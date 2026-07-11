#ifndef SERVICES_OPERATIONS_OP_INTERFACE_H
#define SERVICES_OPERATIONS_OP_INTERFACE_H

#include "common/types.h"

/**
 * Operation Plugin Interface.
 *
 * Each recovery / install / backup procedure is a standalone plugin
 * implementing this interface.  New operations can be added without
 * modifying any existing code – just implement the interface and
 * register the plugin with the service manager.
 */

typedef struct operation_plugin {
    /** Unique short name, e.g. "light_recovery", "deep_install". */
    const char *name;

    /** Human-readable description shown in the UI. */
    const char *description;

    /** Pre-flight check.
     *  @return 0 if prerequisites are met, -1 otherwise. */
    int (*validate)(void);

    /** Initialise the operation with configuration.
     *  @return 0 on success. */
    int (*init)(void);

    /** Execute the operation.
     *  Called from a worker thread.
     *  @param progress  Callback to report progress (0–100) and status text.
     *  @param ctx       Opaque context pointer.
     *  @return operation_result_t describing the outcome. */
    operation_result_t (*execute)(progress_callback_t progress, void *ctx);

    /** Clean up any resources allocated by init() or execute(). */
    void (*cleanup)(void);
} operation_plugin_t;

/**
 * Register an operation plugin so it can be discovered by the service
 * manager and invoked by name.
 */
int operation_plugin_register(const operation_plugin_t *plugin);

/**
 * Look up a registered plugin by name.
 * @return The plugin pointer, or NULL if not found.
 */
const operation_plugin_t *operation_plugin_find(const char *name);

/** De-register all plugins. */
void operation_plugin_deregister_all(void);

/**
 * Convenience macro to auto-register a plugin via __attribute__((constructor)).
 * Place at file scope in each plugin .c file:
 *
 *   REGISTER_OPERATION_PLUGIN(light_recovery, "Lightweight System Recovery",
 *                              validate, init, execute, cleanup);
 */
#define REGISTER_OPERATION_PLUGIN(_name, _desc, _val, _init, _exec, _clean) \
    __attribute__((constructor)) \
    static void _op_register_##_name(void) \
    { \
        static operation_plugin_t _plugin_##_name = { \
            .name        = #_name, \
            .description = _desc, \
            .validate    = _val, \
            .init        = _init, \
            .execute     = _exec, \
            .cleanup     = _clean, \
        }; \
        operation_plugin_register(&_plugin_##_name); \
    }

#endif /* SERVICES_OPERATIONS_OP_INTERFACE_H */
