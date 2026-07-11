#ifndef CORE_APP_CORE_H
#define CORE_APP_CORE_H

#include <stdbool.h>

/**
 * Application Core – manages the overall lifecycle.
 *
 * Usage:
 *   1. app_core_init(&argc, &argv)
 *   2. app_core_run()          ← blocks until shutdown
 *   3. app_core_deinit()
 */

/** One-time initialisation. Parses config, inits HAL, services, UI. */
bool app_core_init(int *argc, char ***argv);

/** Enter the main event loop. Returns when the app should exit. */
void app_core_run(void);

/** Clean up all resources. */
void app_core_deinit(void);

/** Request graceful shutdown. */
void app_core_request_shutdown(void);

/** Request system reboot. */
void app_core_request_reboot(void);

/** Request system poweroff. */
void app_core_request_poweroff(void);

#endif /* CORE_APP_CORE_H */
