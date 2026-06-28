#include "config.h"
#include "logger.h"
#include "scanner.h"
#include "smart_ata.h"
#include "smart_nvme.h"
#include "io_metrics.h"
#include "health_score.h"
#include "json_builder.h"
#include "daemon.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ============================================================
 *  Entry point
 * ============================================================ */

int main(int argc, char *argv[]) {
    config_t cfg;
    config_init(&cfg);

    if (config_parse_args(&cfg, argc, argv) != 0) {
        return 1;
    }

    /* Open logger and configure rotation */
    logger_open(cfg.log_path);
    logger_set_rotate(cfg.log_max_size, cfg.log_rotate_count);
    logger_write(LOG_INFO, "disk_health v1.0.0 starting");

    /* --- One-shot JSON mode --- */
    if (cfg.output_json) {
        int rc = run_json_mode(&cfg);
        logger_close();
        return rc;
    }

    /* --- Foreground watch mode --- */
    if (cfg.watch_mode) {
        printf("disk_health: watch mode (interval=%ds), press Ctrl+C to stop\n",
               cfg.interval_seconds);
        logger_write(LOG_INFO, "watch mode, interval=%ds", cfg.interval_seconds);
        daemon_main_loop(&cfg);
        logger_close();
        return 0;
    }

    /* --- Daemon mode (default) --- */
    /* Check if running as root */
    if (geteuid() != 0) {
        fprintf(stderr, "disk_health: daemon mode requires root privileges\n");
        fprintf(stderr, "Use --json or --watch for non-root operation\n");
        logger_write(LOG_WARN, "Non-root user attempted daemon mode");
        logger_close();
        return 1;
    }

    printf("disk_health: starting daemon (interval=%ds)\n", cfg.interval_seconds);
    daemon_run(&cfg);
    logger_close();
    return 0;
}
