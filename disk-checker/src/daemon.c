#include "daemon.h"
#include "config.h"
#include "logger.h"
#include "scanner.h"
#include "smart_ata.h"
#include "smart_nvme.h"
#include "io_metrics.h"
#include "health_score.h"
#include "json_builder.h"
#include "fault_detect.h"
#include "policy_engine.h"
#include "failure_predict.h"
#include "self_healing.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

/* ============================================================
 *  Global state
 * ============================================================ */

volatile int g_shutdown  = 0;
volatile int g_reload    = 0;
volatile int g_daemonized = 0;

#define PIDFILE_PATH "/var/run/disk_healthd.pid"

/* ============================================================
 *  Signal handler
 * ============================================================ */

void daemon_signal_handler(int sig) {
    switch (sig) {
    case SIGTERM:
    case SIGINT:
        g_shutdown = 1;
        break;
    case SIGHUP:
        g_reload = 1;
        break;
    default:
        break;
    }
}

/* ============================================================
 *  PID file management
 * ============================================================ */

int daemon_write_pidfile(void) {
    FILE *f = fopen(PIDFILE_PATH, "w");
    if (!f) {
        logger_write(LOG_WARN, "Cannot write PID file %s: %s",
                     PIDFILE_PATH, strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

void daemon_remove_pidfile(void) {
    unlink(PIDFILE_PATH);
}

/* ============================================================
 *  Daemonize (double-fork)
 * ============================================================ */

static int daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        /* Parent exits */
        _exit(0);
    }

    /* Child: create new session */
    if (setsid() < 0) {
        fprintf(stderr, "setsid failed: %s\n", strerror(errno));
        return -1;
    }

    /* Fork again to detach from controlling terminal */
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "second fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        _exit(0);
    }

    /* Grandchild: daemon process */
    chdir("/");

    /* Redirect stdin/stdout/stderr to /dev/null */
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > 2) close(null_fd);
    }

    /* Set reasonable umask */
    umask(022);

    g_daemonized = 1;
    return 0;
}

void daemon_run(config_t *cfg) {
    if (daemonize() != 0) {
        fprintf(stderr, "Daemonization failed\n");
        exit(1);
    }

    /* Install signal handlers (after daemonization so they persist) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* No SA_RESTART: sleep() returns EINTR on signal */

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    /* Write PID file */
    daemon_write_pidfile();

    /* Run the main monitoring loop */
    daemon_main_loop(cfg);

    /* Cleanup */
    daemon_remove_pidfile();
    logger_write(LOG_INFO, "disk_health daemon stopped");
}

/* ============================================================
 *  JSON one-shot output
 * ============================================================ */

static void build_device_json(json_builder_t *jb,
                               const device_info_t   *dev,
                               const smart_ata_data_t  *ata,
                               const smart_nvme_data_t *nvme,
                               const io_metrics_t     *io,
                               const health_score_t   *score) {
    json_add_string(jb, "device", dev->path);
    json_add_string(jb, "name", dev->name);
    json_add_string(jb, "model", dev->model);
    json_add_string(jb, "serial", dev->serial);
    json_add_string(jb, "type", dev->type == DEVICE_TYPE_SATA ? "SATA" :
                                 dev->type == DEVICE_TYPE_NVME ? "NVMe" : "UNKNOWN");
    json_add_bool(jb, "rotational", dev->rotational == 1);

    json_add_int(jb, "score", score->score);
    json_add_string(jb, "state", score->state_label);

    if (ata && ata->supported) {
        json_add_int(jb, "temperature", ata->temperature_celsius);
        json_add_uint64(jb, "reallocated_sectors", ata->reallocated_sectors);
        json_add_uint64(jb, "pending_sectors", ata->pending_sectors);
        json_add_uint64(jb, "uncorrectable_errors", ata->uncorrectable_errors);
        json_add_uint64(jb, "crc_errors", ata->crc_errors);
        json_add_uint64(jb, "power_on_hours", ata->power_on_hours);
    }

    if (nvme && nvme->present) {
        int temp_c = (int)nvme->temperature_kelvin - 273;
        json_add_int(jb, "temperature", temp_c);
        json_add_int(jb, "nvme_percentage_used", nvme->percentage_used);
        json_add_uint64(jb, "nvme_media_errors", nvme->media_errors);
    }

    if (io && io->valid) {
        json_add_double(jb, "read_iops", io->read_iops);
        json_add_double(jb, "write_iops", io->write_iops);
        json_add_double(jb, "avg_latency_ms", io->avg_latency_ms);
    }
}

int run_json_mode(config_t *cfg) {
    device_list_t devices;
    int ndev = scan_block_devices(&devices);

    if (ndev <= 0) {
        fprintf(stderr, "{\"error\":\"no devices found\"}\n");
        return 1;
    }

    json_builder_t jb;
    json_init(&jb);
    json_open_object(&jb);
    json_add_int(&jb, "timestamp", (int)time(NULL));
    json_open_array(&jb, "devices");

    for (int i = 0; i < devices.count; i++) {
        device_info_t *dev = &devices.devices[i];

        /* Filter by watch_device if specified */
        if (cfg->watch_device[0] != '\0') {
            if (strcmp(dev->path, cfg->watch_device) != 0) continue;
        }

        json_open_object(&jb);

        smart_ata_data_t  ata  = {0};
        smart_nvme_data_t nvme = {0};
        io_metrics_t      io   = {0};
        health_score_t    score = {0};

        if (dev->type == DEVICE_TYPE_SATA) {
            smart_ata_read(dev->path, &ata);
        } else if (dev->type == DEVICE_TYPE_NVME) {
            smart_nvme_read(dev->path, &nvme);
        }

        io_metrics_read(dev->name, &io);
        health_score_compute(&ata, &nvme, &io, &score);

        build_device_json(&jb, dev, &ata, &nvme, &io, &score);
        json_close_object(&jb);
    }

    json_close_array(&jb);
    json_close_object(&jb);

    printf("%s\n", json_str(&jb));
    return 0;
}

/* ============================================================
 *  Main monitoring loop
 * ============================================================ */

/* Per-device previous IO metrics for delta computation */
static io_metrics_t  prev_io[MAX_DEVICES];
/* V2: Per-device trend data for failure prediction */
static trend_data_t  trend_data[MAX_DEVICES];

void daemon_main_loop(config_t *cfg) {
    /* Install signal handlers for foreground modes too */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* No SA_RESTART for foreground too */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    memset(prev_io, 0, sizeof(prev_io));
    for (int i = 0; i < MAX_DEVICES; i++) {
        trend_data_init(&trend_data[i]);
    }

    /* First pass: collect baseline IO counters */
    device_list_t init_devices;
    int ndev = scan_block_devices(&init_devices);
    for (int i = 0; i < ndev && i < MAX_DEVICES; i++) {
        if (cfg->watch_device[0] != '\0' &&
            strcmp(init_devices.devices[i].path, cfg->watch_device) != 0)
            continue;
        io_metrics_read(init_devices.devices[i].name, &prev_io[i]);
        prev_io[i].valid = 1;
    }

    logger_write(LOG_INFO, "Monitoring loop started (interval=%ds)",
                 cfg->interval_seconds);

    while (!g_shutdown) {
        /* Handle SIGHUP — reload config */
        if (g_reload) {
            logger_write(LOG_INFO, "SIGHUP received, reloading config");
            config_load_file(cfg, "/etc/disk_health/disk_health.conf");
            logger_set_rotate(cfg->log_max_size, cfg->log_rotate_count);
            g_reload = 0;
        }

        device_list_t devices;
        ndev = scan_block_devices(&devices);

        if (ndev <= 0) {
            logger_write(LOG_WARN, "No block devices found");
        }

        for (int i = 0; i < devices.count; i++) {
            device_info_t *dev = &devices.devices[i];

            /* Filter by watch_device if specified */
            if (cfg->watch_device[0] != '\0') {
                if (strcmp(dev->path, cfg->watch_device) != 0) continue;
            }

            smart_ata_data_t  ata  = {0};
            smart_nvme_data_t nvme = {0};
            io_metrics_t      io   = {0};
            health_score_t    score = {0};

            /* Read SMART data */
            if (dev->type == DEVICE_TYPE_SATA) {
                smart_ata_read(dev->path, &ata);
            } else if (dev->type == DEVICE_TYPE_NVME) {
                smart_nvme_read(dev->path, &nvme);
            }

            /* Read and compute IO metrics */
            io_metrics_read(dev->name, &io);
            io_metrics_compute(&io, &prev_io[i], (double)cfg->interval_seconds);
            io_metrics_copy(&io, &prev_io[i]);

            /* Compute health score */
            health_score_compute(&ata, &nvme, &io, &score);

            /* --- V2: Fault Detection --- */
            fault_result_t fault = {0};
            fault_detect(&score, &ata, &nvme, &io,
                         &trend_data[i], &fault);

            /* --- V2: Update trend data for failure prediction --- */
            trend_data_update(&trend_data[i], &ata, &io);

            /* --- V2: Failure Prediction --- */
            predict_result_t predict = {0};
            failure_predict_compute(&trend_data[i], &score, &predict);

            /* --- V2: Policy Decision --- */
            policy_decision_t decision = {0};
            policy_evaluate(&score, &fault, cfg->heal_max_level, &decision);

            /* --- V2: Self-Healing Execution --- */
            if (decision.action != ACTION_NONE) {
                self_healing_execute(&decision, dev, cfg->auto_heal);
                logger_write_json_event(dev->path, fault.level,
                                        decision.action,
                                        score.score,
                                        predict.failure_probability);
            }

            /* Log the scan result */
            logger_write(LOG_INFO,
                         "%s score=%d state=%s temp=%d "
                         "realloc=%lu pending=%lu uncorrect=%lu "
                         "rio=%.1f wio=%.1f lat=%.2fms",
                         dev->path,
                         score.score, score.state_label,
                         ata.temperature_celsius,
                         (unsigned long)ata.reallocated_sectors,
                         (unsigned long)ata.pending_sectors,
                         (unsigned long)ata.uncorrectable_errors,
                         io.read_iops, io.write_iops, io.avg_latency_ms);

            /* Alert on degraded/critical states */
            if (score.state == HEALTH_WARNING || score.state == HEALTH_DEGRADED) {
                logger_write(LOG_WARN,
                             "Device %s is %s (score=%d) — check recommended",
                             dev->path, score.state_label, score.score);
            } else if (score.state == HEALTH_CRITICAL) {
                logger_write(LOG_CRITICAL,
                             "CRITICAL: Device %s score=%d — "
                             "immediate attention required! Potential failure predicted.",
                             dev->path, score.score);
            }
        }

        /* Sleep, handling early return from signals (EINTR) */
        unsigned int remaining = (unsigned int)cfg->interval_seconds;
        while (remaining > 0 && !g_shutdown) {
            remaining = sleep(remaining);
        }
    }

    logger_write(LOG_INFO, "Monitoring loop stopped (shutdown signal)");
}
