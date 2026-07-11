/**
 * @file main.c
 * @brief System Recovery – Application Entry Point
 *
 * Embedded Linux system recovery/installation tool with LVGL GUI.
 * Targets ARM64 (aarch64) platforms with Linux framebuffer.
 *
 * Architecture:
 *   Layer 1: HAL (display, input, storage)
 *   Layer 2: Core (event bus, app lifecycle)
 *   Layer 3: Services (config, log, recovery/install/backup, operation plugins)
 *   Layer 4: UI (screen manager, screens)
 */

#include "core/app_core.h"
#include <stdio.h>

int main(int argc, char *argv[])
{
    printf("=== System Recovery v2 ===\n");
    printf("Build: %s\n", "20260711-arch-v2");

    /* Initialise all subsystems */
    if (!app_core_init(&argc, &argv)) {
        fprintf(stderr, "FATAL: initialisation failed\n");
        return 1;
    }

    /* Main event loop (blocks until shutdown) */
    app_core_run();

    /* Cleanup */
    app_core_deinit();

    printf("=== System Recovery exited ===\n");
    return 0;
}
