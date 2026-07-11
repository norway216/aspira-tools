#include "app_core.h"
#include "event_bus.h"
#include "common/types.h"
#include "common/utils.h"
#include "common/version.h"
#include "hal/display/display.h"
#include "hal/input/input_manager.h"
#include "services/config_manager.h"
#include "services/log_service.h"
#include "services/service_manager.h"
#include "ui/ui_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>

#define MAX_POLL_FDS  32

static volatile bool g_running   = true;
static volatile bool g_reboot    = false;
static volatile bool g_poweroff  = false;

/* ---- Event handlers --------------------------------------------------- */

static void on_shutdown(const event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    g_running = false;
}

static void on_reboot(const event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    g_running = false;
    g_reboot = true;
}

static void on_poweroff(const event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    g_running = false;
    g_poweroff = true;
}

/* ---- Public API ------------------------------------------------------- */

bool app_core_init(int *argc, char ***argv)
{
    (void)argc; (void)argv;

    printf("System Recovery %s (build %s)\n",
           get_version(), get_build_id());

    /* 1. Event bus */
    event_bus_init();

    /* 2. Configuration */
    if (!config_manager_init()) {
        fprintf(stderr, "app: config init failed, using defaults\n");
    }

    /* 3. Logging */
    if (!log_service_init()) {
        fprintf(stderr, "app: log service init failed\n");
    }
    log_service_write(LOG_LEVEL_INFO, "System Recovery starting...");

    /* 4. Display (framebuffer) */
    if (!display_init()) {
        log_service_write(LOG_LEVEL_ERROR, "Display init failed");
        return false;
    }

    /* 5. Input manager */
    if (!input_manager_init()) {
        log_service_write(LOG_LEVEL_WARN, "Input init failed — continuing without input");
    }

    /* 6. Service manager (recovery/install/backup services) */
    if (!service_manager_init()) {
        log_service_write(LOG_LEVEL_ERROR, "Service manager init failed");
        return false;
    }

    /* 7. UI Manager */
    if (!ui_manager_init()) {
        log_service_write(LOG_LEVEL_ERROR, "UI manager init failed");
        return false;
    }

    /* Subscribe to lifecycle events */
    event_bus_subscribe(EVENT_SHUTDOWN, on_shutdown, NULL);
    event_bus_subscribe(EVENT_REBOOT,   on_reboot,   NULL);
    event_bus_subscribe(EVENT_APP_QUIT, on_poweroff, NULL);

    log_service_write(LOG_LEVEL_INFO, "Initialisation complete");
    return true;
}

void app_core_run(void)
{
    printf("app: entering main loop\n");

    struct pollfd pfds[MAX_POLL_FDS];

    while (g_running) {
        /* Let LVGL process UI tasks and get delay to next timer */
        uint32_t lvgl_delay = ui_manager_tick();

        /* Collect input FDs for polling */
        int input_fds[MAX_POLL_FDS];
        int nfds = input_manager_get_poll_fds(input_fds, MAX_POLL_FDS);

        for (int i = 0; i < nfds; i++) {
            pfds[i].fd     = input_fds[i];
            pfds[i].events = POLLIN;
        }

        /* Poll with timeout = min(LVGL timer delay, 100 ms fallback) */
        int timeout = (int)lvgl_delay;
        if (timeout <= 0 || timeout > 100) timeout = 20;  /* 20 ms default */

        int ret = poll(pfds, (nfds_t)nfds, timeout);

        if (ret > 0 || (ret == 0 && nfds == 0)) {
            /* Data available, or no FDs to monitor — poll input */
            input_manager_poll();
        } else if (ret == 0) {
            /* Timeout with active FDs — poll input anyway to catch
             * polling-type devices that don't use standard evdev */
            input_manager_poll();
        } else {
            /* ret < 0: EINTR or other error — small delay to avoid
             * busy-loop on persistent errors, then poll input */
            if (errno == EINTR) {
                usleep(1000);  /* 1 ms back-off */
            }
            input_manager_poll();
        }
    }

    printf("app: exiting main loop\n");

    if (g_reboot) {
        printf("app: rebooting...\n");
        utils_shell_exec("sync");
        utils_shell_exec("reboot -f");
    } else if (g_poweroff) {
        printf("app: powering off...\n");
        utils_shell_exec("sync");
        utils_shell_exec("poweroff -f");
    }
}

void app_core_deinit(void)
{
    ui_manager_deinit();
    service_manager_deinit();
    input_manager_deinit();
    display_deinit();
    log_service_deinit();
    config_manager_deinit();
    event_bus_deinit();
}

void app_core_request_shutdown(void)  { event_bus_publish_int(EVENT_SHUTDOWN, 0); }
void app_core_request_reboot(void)    { event_bus_publish_int(EVENT_REBOOT, 0); }
void app_core_request_poweroff(void)  { event_bus_publish_int(EVENT_APP_QUIT, 0); }
