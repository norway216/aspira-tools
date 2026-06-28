/*
 * scanner.c — TCP Connect Scan Engine with Target Queue
 *
 * Non-blocking TCP connect state machine driven by epoll events:
 *
 *   socket() + connect() [non-blocking]
 *     │
 *     ▼
 *   INIT ──────────────────────────────► CONNECTING
 *                                            │
 *                     ┌──────────────────────┼──────────────────────┐
 *                     │ EPOLLERR/EPOLLHUP    │ EPOLLOUT             │ timer fires
 *                     ▼                      ▼                      ▼
 *                   ERROR ─► CLOSED        OPEN ─► CLOSED        TIMEOUT ─► CLOSED
 *                                            │
 *                                     [read banner + fp_match()]
 *                                            │
 *                                            ▼
 *                                   push result → ring_queue
 *                                   close fd → fd_mgr_release
 *                                   drain target queue → submit next
 *
 * Target Queue:
 *   All targets from scanner_submit_targets() are stored in a queue.
 *   The scanner drains the queue up to the concurrency limit. When a
 *   scan completes and a slot frees up, the next target is dequeued
 *   and started. This continues until the queue is empty AND all
 *   in-flight scans have completed — then the reactor is stopped.
 */

#include "scanner.h"
#include "../core/reactor.h"
#include "../core/timer_wheel.h"
#include "../net/socket.h"
#include "../fp/fingerprint.h"
#include "../result/queue.h"
#include <stdlib.h>

/* ============================================================
 *  Internal globals (one scanner instance per process)
 * ============================================================ */

static reactor_t      *g_reactor        = NULL;
static ring_queue_t   *g_queue          = NULL;
static scan_callback_t g_callback       = NULL;
static void           *g_cb_ctx         = NULL;
static int             g_pending        = 0;
static int             g_timeout_ticks  = 0;

/* Target queue */
static scan_target_t  *g_targets        = NULL;
static int             g_target_head    = 0;    /* next to dequeue */
static int             g_target_tail    = 0;    /* next write slot */
static int             g_target_capacity = 0;
static int             g_total_submitted = 0;
static int             g_total_completed = 0;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

int
scanner_init(reactor_t *r, ring_queue_t *queue,
             scan_callback_t cb, void *ctx)
{
    g_reactor   = r;
    g_queue     = queue;
    g_callback  = cb;
    g_cb_ctx    = ctx;
    g_pending   = 0;
    g_timeout_ticks = 0;

    /* Allocate target queue (capacity = max targets) */
    int cap = NA_MAX_TARGETS;
    g_targets = (scan_target_t *)calloc((size_t)cap, sizeof(scan_target_t));
    if (!g_targets) return -1;
    g_target_capacity = cap;
    g_target_head = 0;
    g_target_tail = 0;
    g_total_submitted = 0;
    g_total_completed = 0;

    return 0;
}

void
scanner_cleanup(void)
{
    free(g_targets);
    g_targets     = NULL;
    g_reactor     = NULL;
    g_queue       = NULL;
    g_callback    = NULL;
    g_cb_ctx      = NULL;
    g_pending     = 0;
    g_target_capacity = 0;
}

/* ============================================================
 *  Target queue helpers (simple ring buffer)
 * ============================================================ */

static int
target_queue_empty(void)
{
    return g_target_head == g_target_tail;
}

static int
target_queue_full(void)
{
    return ((g_target_tail + 1) % g_target_capacity) == g_target_head;
}

static int
target_enqueue(const scan_target_t *t)
{
    if (target_queue_full()) return -1;
    g_targets[g_target_tail] = *t;
    g_target_tail = (g_target_tail + 1) % g_target_capacity;
    return 0;
}

static int
target_dequeue(scan_target_t *t)
{
    if (target_queue_empty()) return -1;
    *t = g_targets[g_target_head];
    g_target_head = (g_target_head + 1) % g_target_capacity;
    return 0;
}

/* ============================================================
 *  Start a single scan for one target
 *  Returns 0 on success, -1 if at concurrency limit, -2 on error
 * ============================================================ */

static int
scanner_start_one(const scan_target_t *target)
{
    fd_manager_t *fdm = reactor_get_fd_mgr(g_reactor);

    /* Check FD capacity */
    if (fd_mgr_acquire(fdm) < 0) {
        return -1;
    }

    /* Allocate socket entry */
    socket_entry_t *se = reactor_socket_alloc(g_reactor);
    if (!se) {
        fd_mgr_release(fdm);
        return -1;
    }

    /* Create non-blocking socket */
    int fd;
    if (sk_create(&fd) < 0) {
        reactor_socket_free_entry(g_reactor, se);
        fd_mgr_release(fdm);
        return -2;
    }

    /* Set TCP_NODELAY for low-latency banner reads */
    sk_set_nodelay(fd);

    /* Initiate non-blocking connect */
    int ret = sk_connect_nonblock(fd, target->ip, target->port);
    if (ret < 0) {
        /* Immediate failure */
        close(fd);
        reactor_socket_free_entry(g_reactor, se);
        fd_mgr_release(fdm);
        return -2;
    }

    /* Populate socket entry */
    se->fd         = fd;
    se->state      = SK_CONNECTING;
    se->target     = *target;
    se->start_tick = (int64_t)tw_current_tick(reactor_get_timer(g_reactor));
    se->rtt_ms     = -1;
    se->banner_len = 0;
    se->timer_id   = -1;
    se->in_use     = 1;

    /* Register with epoll (waiting for EPOLLOUT) */
    if (reactor_add_fd(g_reactor, fd, EPOLLOUT, se) < 0) {
        close(fd);
        reactor_socket_free_entry(g_reactor, se);
        fd_mgr_release(fdm);
        return -2;
    }

    /* Add timeout timer */
    timer_wheel_t *tw = reactor_get_timer(g_reactor);
    int timeout_ticks = na_ms_to_ticks(g_timeout_ticks > 0 ?
                                       g_timeout_ticks : NA_TIMEOUT_DEF_MS);
    g_timeout_ticks = timeout_ticks;
    se->timer_id = tw_add(tw, timeout_ticks, scanner_on_timeout, se);

    g_pending++;
    return 0;
}

/* ============================================================
 *  Drain target queue — start as many scans as possible
 * ============================================================ */

static void
scanner_drain_queue(void)
{
    while (!target_queue_empty()) {
        /* Peek at next target (don't dequeue until started) */
        scan_target_t target = g_targets[g_target_head];

        int rc = scanner_start_one(&target);
        if (rc == -1) {
            /* At concurrency limit — stop, will retry when slots free */
            break;
        }
        /* Successfully started (rc == 0) or error (rc == -2).
         * In both cases, dequeue and move on. */
        target_dequeue(&target);
        if (rc == -2) {
            g_total_completed++;
        }
    }
}

/* ============================================================
 *  Target submission — enqueue all targets, then drain
 * ============================================================ */

int
scanner_submit_targets(const scan_target_t *targets, int count)
{
    if (!g_reactor || !g_targets) return -1;

    /* Enqueue all targets */
    for (int i = 0; i < count; i++) {
        if (target_enqueue(&targets[i]) < 0) {
            break;
        }
        g_total_submitted++;
    }

    /* Drain queue up to concurrency limit */
    scanner_drain_queue();

    return g_total_submitted;
}

/* ============================================================
 *  Event handler — called by reactor for every epoll event
 * ============================================================ */

void
scanner_on_event(struct epoll_event *ev, void *ctx)
{
    (void)ctx;

    socket_entry_t *se = (socket_entry_t *)ev->data.ptr;
    if (!se || !se->in_use) {
        return;
    }

    uint32_t events = ev->events;

    /* Error conditions take priority */
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        scanner_on_error(se);
        return;
    }

    /* State-specific handling */
    switch (se->state) {
    case SK_CONNECTING:
        if (events & EPOLLOUT) {
            scanner_on_connect_done(se);
        }
        break;

    case SK_OPEN:
        if (events & EPOLLIN) {
            scanner_on_readable(se);
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 *  State: CONNECTING → OPEN (or CLOSED on error)
 * ============================================================ */

void
scanner_on_connect_done(socket_entry_t *se)
{
    int err = sk_get_error(se->fd);

    if (err == 0) {
        /* Connection successful */
        timer_wheel_t *tw = reactor_get_timer(g_reactor);

        /* Cancel timeout timer */
        if (se->timer_id > 0) {
            tw_cancel(tw, se->timer_id);
            se->timer_id = -1;
        }

        /* Calculate RTT */
        se->rtt_ms = (int)(tw_current_tick(tw) - se->start_tick) * NA_TICK_MS;
        if (se->rtt_ms < 0) se->rtt_ms = 0;

        /* Transition to OPEN */
        se->state = SK_OPEN;

        /* Switch epoll to wait for readable data (banner) */
        reactor_mod_fd(g_reactor, se->fd, EPOLLIN, se);

        /* Add read timeout: 2 seconds to receive banner */
        timer_wheel_t *tw2 = reactor_get_timer(g_reactor);
        se->timer_id = tw_add(tw2, na_ms_to_ticks(2000), scanner_on_timeout, se);
    } else {
        /* Connection failed */
        scanner_finalize(se, SK_CLOSED);
    }
}

/* ============================================================
 *  State: OPEN → banner read → fingerprint match → CLOSED
 * ============================================================ */

void
scanner_on_readable(socket_entry_t *se)
{
    char *buf = se->banner + se->banner_len;
    int   remaining = (int)sizeof(se->banner) - se->banner_len - 1;

    if (remaining <= 0) {
        scanner_finalize(se, SK_OPEN);
        return;
    }

    int n = sk_read(se->fd, buf, (size_t)remaining);

    if (n > 0) {
        se->banner_len += n;
        se->banner[se->banner_len] = '\0';

        /* Try fingerprint match */
        fp_service_t svc = fp_match(se->banner, se->banner_len);
        if (svc != FP_UNKNOWN) {
            scanner_finalize(se, SK_OPEN);
            return;
        }

        /* Enough data without a match — close anyway */
        if (se->banner_len >= 64) {
            scanner_finalize(se, SK_OPEN);
        }
    } else if (n == 0) {
        /* EOF (peer closed) */
        scanner_finalize(se, SK_OPEN);
    }
    /* n < 0 with EAGAIN: wait for next epoll event */
}

/* ============================================================
 *  Timeout callback (fired by timer wheel)
 * ============================================================ */

void
scanner_on_timeout(void *arg)
{
    socket_entry_t *se = (socket_entry_t *)arg;

    if (!se || !se->in_use) return;

    se->timer_id = -1;

    if (se->state == SK_CONNECTING) {
        scanner_finalize(se, SK_TIMEOUT);
    } else if (se->state == SK_OPEN) {
        scanner_finalize(se, SK_OPEN);
    }
}

/* ============================================================
 *  Error handling
 * ============================================================ */

void
scanner_on_error(socket_entry_t *se)
{
    scanner_finalize(se, SK_ERROR);
}

/* ============================================================
 *  Finalize: push result, close fd, release resources,
 *            drain target queue to refill slot
 * ============================================================ */

void
scanner_finalize(socket_entry_t *se, sk_state_t final_state)
{
    if (!se || !se->in_use) return;

    se->state = final_state;

    /* Cancel any pending timer */
    if (se->timer_id > 0) {
        tw_cancel(reactor_get_timer(g_reactor), se->timer_id);
        se->timer_id = -1;
    }

    /* Remove from epoll */
    if (se->fd >= 0) {
        reactor_del_fd(g_reactor, se->fd);
    }

    /* Build result */
    scan_result_t result;
    memset(&result, 0, sizeof(result));
    result.target       = se->target;
    result.state        = se->state;
    result.rtt_ms       = se->rtt_ms;
    result.timestamp_ms = na_now_ms();

    if (se->banner_len > 0) {
        fp_service_t svc = fp_match(se->banner, se->banner_len);
        strncpy(result.service, fp_service_name(svc), sizeof(result.service) - 1);
        strncpy(result.banner, se->banner, sizeof(result.banner) - 1);
    } else {
        strncpy(result.service, "unknown", sizeof(result.service) - 1);
    }

    /* Push to ring buffer */
    if (g_queue) {
        ring_push(g_queue, &result);
    }

    /* Invoke callback */
    if (g_callback) {
        g_callback(&result, g_cb_ctx);
    }

    /* Close socket */
    if (se->fd >= 0) {
        close(se->fd);
        se->fd = -1;
    }

    /* Release FD slot */
    fd_mgr_release(reactor_get_fd_mgr(g_reactor));

    /* Mark slot free */
    se->in_use = 0;
    g_pending--;
    g_total_completed++;

    /*
     * Refill: drain the target queue to start new scans in the
     * freed slot. Then check if we're completely done.
     */
    scanner_drain_queue();

    if (g_pending <= 0 && target_queue_empty() && g_reactor) {
        reactor_stop(g_reactor);
    }
}

/* ============================================================
 *  Status
 * ============================================================ */

int
scanner_pending(void)
{
    return g_pending;
}
