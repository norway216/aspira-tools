/*
 * main.c — Entry Point for Lightweight Network Audit Framework V1
 *
 * Orchestrates the full scan pipeline:
 *   1. Parse CLI arguments
 *   2. Initialize subsystems (reactor, scanner, fingerprint DB, queues)
 *   3. Expand targets from --target argument
 *   4. Submit targets to scanner
 *   5. Run reactor event loop
 *   6. Drain and display results
 *   7. Cleanup
 *
 * Security: Internal lab networks only. See doc for constraints.
 */

#include "../include/net_audit.h"
#include "config.h"
#include "../core/reactor.h"
#include "../core/timer_wheel.h"
#include "../net/socket.h"
#include "../scan/scanner.h"
#include "../fp/fingerprint.h"
#include "../result/queue.h"
#include "../worker/pool.h"
#include "../db/sqlite.h"

/* ============================================================
 *  Signal handler
 * ============================================================ */

static void
sig_handler(int sig)
{
    (void)sig;
    atomic_store(&g_shutdown, 1);
}

static int
setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags   = SA_RESTART;
    if (sigaction(SIGINT,  &sa, NULL) < 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;
    /* Ignore SIGPIPE — we handle EPIPE via epoll */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* ============================================================
 *  Result display callbacks
 * ============================================================ */

static void
print_result_text(const scan_result_t *r)
{
    char ip_str[NA_IP_STR_LEN];
    na_ip_to_str(r->target.ip, ip_str, sizeof(ip_str));

    /* RTT display: only meaningful for successful connections */
    char rtt_str[16];
    if (r->rtt_ms >= 0) {
        snprintf(rtt_str, sizeof(rtt_str), "%dms", r->rtt_ms);
    } else {
        snprintf(rtt_str, sizeof(rtt_str), "N/A");
    }

    printf("%-16s %-6u %-10s %-10s %6s  %s\n",
           ip_str,
           na_port_host(r->target.port),
           sk_state_label(r->state),
           r->service,
           rtt_str,
           r->banner[0] ? r->banner : "-");
}

static void
print_result_json(const scan_result_t *r)
{
    char ip_str[NA_IP_STR_LEN];
    na_ip_to_str(r->target.ip, ip_str, sizeof(ip_str));

    /* RTT: only meaningful for OPEN connections */
    if (r->rtt_ms >= 0) {
        printf("{\"ip\":\"%s\",\"port\":%u,\"state\":\"%s\","
               "\"rtt_ms\":%d,\"service\":\"%s\",\"banner\":\"%s\"}\n",
               ip_str,
               na_port_host(r->target.port),
               sk_state_label(r->state),
               r->rtt_ms,
               r->service,
               r->banner);
    } else {
        printf("{\"ip\":\"%s\",\"port\":%u,\"state\":\"%s\","
               "\"rtt_ms\":null,\"service\":\"%s\",\"banner\":\"%s\"}\n",
               ip_str,
               na_port_host(r->target.port),
               sk_state_label(r->state),
               r->service,
               r->banner);
    }
}

/* ============================================================
 *  Result callback (called from reactor thread for each result)
 * ============================================================ */

typedef struct {
    na_config_t      *cfg;
    int               total;
    int               open_count;
    int               closed_count;
    int               timeout_count;
    int               error_count;
} stats_ctx_t;

static void
result_callback(const scan_result_t *result, void *ctx)
{
    stats_ctx_t *stats = (stats_ctx_t *)ctx;

    stats->total++;

    switch (result->state) {
    case SK_OPEN:    stats->open_count++;    break;
    case SK_CLOSED:  stats->closed_count++;  break;
    case SK_TIMEOUT: stats->timeout_count++; break;
    case SK_ERROR:   stats->error_count++;   break;
    default: break;
    }

    /* Print result immediately (real-time output) */
    if (stats->cfg->output_fmt == OUTPUT_JSON) {
        print_result_json(result);
    } else {
        print_result_text(result);
    }
    fflush(stdout);

    /* Store to DB if enabled */
    if (stats->cfg->use_db) {
        asset_row_t row;
        memset(&row, 0, sizeof(row));
        na_ip_to_str(result->target.ip, row.ip, sizeof(row.ip));
        row.port     = na_port_host(result->target.port);
        strncpy(row.service, result->service, sizeof(row.service) - 1);
        row.state    = (int)result->state;
        row.last_seen = (int64_t)time(NULL);
        /* Fix #19: check return value */
        if (db_store_asset(&row) < 0) {
            fprintf(stderr, "Warning: failed to store asset to DB\n");
        }
    }
}

/* ============================================================
 *  Main
 * ============================================================ */

int
main(int argc, char *argv[])
{
    /* ---- 1. Parse config ---- */
    na_config_t cfg;
    config_init(&cfg);

    int ret = config_parse_args(&cfg, argc, argv);
    if (ret < 0) {
        return 1;
    }
    if (ret > 0) {
        return 0;                      /* --help printed */
    }

    /* ---- 2. Security banner ---- */
    fprintf(stderr,
            "=== Lightweight Network Audit Framework V1 ===\n"
            "=== INTERNAL LAB USE ONLY ===\n"
            "Target: %s  Ports: %d-%d  Concurrency: %d  Timeout: %dms\n\n",
            cfg.target_arg, cfg.port_start, cfg.port_end,
            cfg.concurrency, cfg.timeout_ms);

    /* ---- 3. Setup signals ---- */
    if (setup_signals() < 0) {
        fprintf(stderr, "Warning: signal setup failed, Ctrl-C may not work\n");
    }

    /* ---- 4. Initialize fingerprint DB ---- */
    fp_init_default(&g_fp_db);

    /* ---- 5. Initialize reactor ---- */
    reactor_t reactor;
    if (reactor_init(&reactor, cfg.concurrency) < 0) {
        fprintf(stderr, "Fatal: failed to initialize reactor\n");
        return 1;
    }

    /* ---- 6. Initialize result queue ---- */
    ring_queue_t result_queue;
    ring_init(&result_queue);

    /* ---- 7. Initialize worker pool (optional) ---- */
    worker_pool_t pool;
    ring_queue_t  worker_queue_in;
    ring_queue_t  worker_queue_out;
    memset(&pool, 0, sizeof(pool));

    if (cfg.worker_threads > 0) {
        ring_init(&worker_queue_in);
        ring_init(&worker_queue_out);
        if (pool_init(&pool, cfg.worker_threads,
                      &worker_queue_in, &worker_queue_out) < 0) {
            fprintf(stderr, "Warning: failed to init worker pool, "
                    "running single-threaded\n");
            cfg.worker_threads = 0;
        } else {
            pool_start(&pool);
        }
    }

    /* ---- 8. Open database (optional) ---- */
    if (cfg.use_db) {
        if (db_open(cfg.db_path) < 0) {
            fprintf(stderr, "Warning: failed to open database '%s'\n",
                    cfg.db_path);
            cfg.use_db = 0;
        }
    }

    /* ---- 9. Expand targets ---- */
    scan_target_t targets[NA_MAX_TARGETS];
    int n_targets = config_expand_targets(&cfg, targets, NA_MAX_TARGETS);
    if (n_targets < 0) {
        reactor_cleanup(&reactor);
        return 1;
    }
    if (n_targets == 0) {
        fprintf(stderr, "No targets to scan.\n");
        reactor_cleanup(&reactor);
        return 0;
    }

    /* ---- 10. Initialize scanner ---- */
    stats_ctx_t stats;
    memset(&stats, 0, sizeof(stats));
    stats.cfg = &cfg;

    if (scanner_init(&reactor, &result_queue, result_callback, &stats) < 0) {
        fprintf(stderr, "Fatal: failed to initialize scanner\n");
        reactor_cleanup(&reactor);
        return 1;
    }

    /* ---- 11. Print output header ---- */
    if (cfg.output_fmt == OUTPUT_TEXT) {
        printf("%-16s %-6s %-10s %-10s %6s  %s\n",
               "IP", "Port", "State", "Service", "RTT", "Banner");
        printf("%-16s %-6s %-10s %-10s %6s  %s\n",
               "----------------", "------", "----------",
               "----------", "------", "------");
    }

    /* ---- 12. Submit targets in batches ---- */
    fprintf(stderr, "Submitting %d targets...\n", n_targets);

    int64_t t_start = na_now_ms();
    scanner_set_timeout_ms(cfg.timeout_ms);
    int submitted = scanner_submit_targets(targets, n_targets);
    if (submitted < n_targets) {
        fprintf(stderr,
                "Warning: only %d/%d targets submitted "
                "(concurrency limit reached)\n",
                submitted, n_targets);
    }

    /* ---- 13. Run reactor event loop ---- */
    fprintf(stderr, "Scanning... (Ctrl-C to stop)\n");

    reactor_run(&reactor, scanner_on_event, &reactor);

    /* ---- 14. Drain remaining results ---- */
    int64_t t_end = na_now_ms();
    int elapsed_ms = (int)(t_end - t_start);

    /* ---- 15. Print summary ---- */
    fprintf(stderr, "\n=== Scan Complete ===\n");
    fprintf(stderr, "Elapsed:    %.3f seconds\n",
            (double)elapsed_ms / 1000.0);
    fprintf(stderr, "Total:      %d\n", stats.total);
    fprintf(stderr, "  Open:     %d\n", stats.open_count);
    fprintf(stderr, "  Closed:   %d\n", stats.closed_count);
    fprintf(stderr, "  Timeout:  %d\n", stats.timeout_count);
    fprintf(stderr, "  Error:    %d\n", stats.error_count);

    if (cfg.use_db) {
        fprintf(stderr, "DB records: %d\n", db_asset_count());
    }

    /* ---- 16. Cleanup ---- */
    scanner_cleanup();

    if (cfg.worker_threads > 0) {
        pool_stop(&pool);
        pool_cleanup(&pool);
    }

    if (cfg.use_db) {
        db_close();
    }

    reactor_cleanup(&reactor);

    return 0;
}
