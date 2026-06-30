/*
 * scanner.c — TCP Connect Scan Engine with Target Queue
 *
 * Non-blocking TCP connect state machine driven by epoll events.
 * States: INIT → CONNECTING → OPEN/CLOSED/TIMEOUT/ERROR
 *
 * Target Queue: all targets enqueued on submission, drained up to
 * concurrency limit.  When a scan completes (or fails immediately),
 * the queue is drained again.  Reactor stops when queue empty and
 * no scans pending.
 */

#include "scanner.h"
#include "../core/reactor.h"
#include "../core/timer_wheel.h"
#include "../net/socket.h"
#include "../fp/fingerprint.h"
#include "../result/queue.h"
#include <stdlib.h>

/* ---- Internal globals ---- */

static reactor_t      *g_reactor         = NULL;
static ring_queue_t   *g_queue           = NULL;
static scan_callback_t g_callback        = NULL;
static void           *g_cb_ctx          = NULL;
static int             g_pending         = 0;
static int             g_timeout_ms      = 0;

static scan_target_t  *g_targets         = NULL;
static int             g_target_head     = 0;
static int             g_target_tail     = 0;
static int             g_target_capacity = 0;
static int             g_total_completed = 0;

#define READ_TIMEOUT_MS 2000

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
    g_timeout_ms = 0;

    g_targets = (scan_target_t *)calloc((size_t)NA_MAX_TARGETS,
                                        sizeof(scan_target_t));
    if (!g_targets) return -1;
    g_target_capacity = NA_MAX_TARGETS;
    g_target_head = 0;
    g_target_tail = 0;

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
}

/* ============================================================
 *  Target queue helpers
 * ============================================================ */

static int tq_empty(void) {
    return g_target_head == g_target_tail;
}
static int tq_full(void) {
    return ((g_target_tail + 1) % g_target_capacity) == g_target_head;
}
static int tq_enq(const scan_target_t *t) {
    if (tq_full()) return -1;
    g_targets[g_target_tail] = *t;
    g_target_tail = (g_target_tail + 1) % g_target_capacity;
    return 0;
}
static int tq_deq(scan_target_t *t) {
    if (tq_empty()) return -1;
    *t = g_targets[g_target_head];
    g_target_head = (g_target_head + 1) % g_target_capacity;
    return 0;
}

static int timeout_ticks(void) {
    int ms = (g_timeout_ms > 0) ? g_timeout_ms : NA_TIMEOUT_DEF_MS;
    return na_ms_to_ticks(ms);
}

/* ============================================================
 *  Emit a result directly (used when connect fails immediately)
 * ============================================================ */
static void
emit_result(const scan_target_t *target, sk_state_t state)
{
    scan_result_t r;
    memset(&r, 0, sizeof(r));
    r.target       = *target;
    r.state        = state;
    r.rtt_ms       = 0;
    r.timestamp_ms = na_now_ms();
    snprintf(r.service, sizeof(r.service), "unknown");

    if (g_queue)    ring_push(g_queue, &r);
    if (g_callback) g_callback(&r, g_cb_ctx);
    g_total_completed++;
}

/* ============================================================
 *  Start one scan.  Returns 0 on success (including immediate
 *  failure), -1 if at concurrency limit.
 * ============================================================ */
static int
scanner_start_one(const scan_target_t *target)
{
    fd_manager_t *fdm = reactor_get_fd_mgr(g_reactor);

    if (fd_mgr_acquire(fdm) < 0)
        return -1;

    socket_entry_t *se = reactor_socket_alloc(g_reactor);
    if (!se) {
        fd_mgr_release(fdm);
        return -1;
    }

    int fd;
    if (sk_create(&fd) < 0) {
        reactor_socket_free_entry(g_reactor, se);
        fd_mgr_release(fdm);
        emit_result(target, SK_ERROR);
        return 0;
    }

    (void)sk_set_nodelay(fd);

    int ret = sk_connect_nonblock(fd, target->ip, target->port);
    if (ret < 0) {
        sk_state_t st = (errno == ECONNREFUSED) ? SK_CLOSED : SK_ERROR;
        close(fd);
        reactor_socket_free_entry(g_reactor, se);
        fd_mgr_release(fdm);
        emit_result(target, st);
        return 0;
    }

    /* Async connect in progress */
    se->fd         = fd;
    se->state      = SK_CONNECTING;
    se->target     = *target;
    se->start_tick = (int64_t)tw_current_tick(reactor_get_timer(g_reactor));
    se->rtt_ms     = -1;
    se->banner_len = 0;
    se->timer_id   = -1;
    se->in_use     = 1;

    if (reactor_add_fd(g_reactor, fd, EPOLLOUT, se) < 0) {
        se->fd = -1;
        close(fd);
        reactor_socket_free_entry(g_reactor, se);
        fd_mgr_release(fdm);
        emit_result(target, SK_ERROR);
        return 0;
    }

    timer_wheel_t *tw = reactor_get_timer(g_reactor);
    se->timer_id = tw_add(tw, timeout_ticks(), scanner_on_timeout, se);
    if (se->timer_id < 0) {
        reactor_del_fd(g_reactor, fd);
        se->fd = -1;
        close(fd);
        reactor_socket_free_entry(g_reactor, se);
        fd_mgr_release(fdm);
        emit_result(target, SK_ERROR);
        return 0;
    }

    g_pending++;
    return 0;
}

/* ============================================================
 *  Drain queue until empty or concurrency saturated
 * ============================================================ */
static void
scanner_drain_queue(void)
{
    scan_target_t t;
    while (!tq_empty()) {
        t = g_targets[g_target_head];  /* peek */
        int rc = scanner_start_one(&t);
        if (rc == -1) break;           /* concurrency saturated */
        tq_deq(&t);
    }
}

/* ============================================================
 *  Submit all targets
 * ============================================================ */
int
scanner_submit_targets(const scan_target_t *targets, int count)
{
    if (!g_reactor || !g_targets) return -1;

    for (int i = 0; i < count; i++)
        tq_enq(&targets[i]);

    scanner_drain_queue();
    return count;
}

void
scanner_set_timeout_ms(int ms)
{
    g_timeout_ms = ms;
}

/* ============================================================
 *  epoll event handler
 * ============================================================ */
void
scanner_on_event(struct epoll_event *ev, void *ctx)
{
    (void)ctx;
    socket_entry_t *se = (socket_entry_t *)ev->data.ptr;
    if (!se || !se->in_use) return;

    uint32_t events = ev->events;

    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        scanner_on_error(se);
        return;
    }

    switch (se->state) {
    case SK_CONNECTING: if (events & EPOLLOUT) scanner_on_connect_done(se); break;
    case SK_OPEN:       if (events & EPOLLIN)  scanner_on_readable(se);   break;
    default: break;
    }
}

/* ============================================================
 *  CONNECTING → OPEN or CLOSED
 * ============================================================ */
void
scanner_on_connect_done(socket_entry_t *se)
{
    int err = sk_get_error(se->fd);

    if (err == 0) {
        timer_wheel_t *tw = reactor_get_timer(g_reactor);
        if (se->timer_id > 0) { tw_cancel(tw, se->timer_id); se->timer_id = -1; }

        se->rtt_ms = (int)(tw_current_tick(tw) - se->start_tick) * NA_TICK_MS;
        if (se->rtt_ms < 0) se->rtt_ms = 0;
        se->state = SK_OPEN;

        if (reactor_mod_fd(g_reactor, se->fd, EPOLLIN, se) < 0) {
            scanner_finalize(se, SK_OPEN);
            return;
        }

        se->timer_id = tw_add(tw, na_ms_to_ticks(READ_TIMEOUT_MS), scanner_on_timeout, se);
        if (se->timer_id < 0) { scanner_finalize(se, SK_OPEN); return; }
    } else {
        scanner_finalize(se, SK_CLOSED);
    }
}

/* ============================================================
 *  OPEN → read banner → fingerprint → CLOSED
 * ============================================================ */
void
scanner_on_readable(socket_entry_t *se)
{
    size_t rem = sizeof(se->banner) - (size_t)se->banner_len - 1;
    if (rem == 0 || se->banner_len >= (int)sizeof(se->banner) - 1) {
        scanner_finalize(se, SK_OPEN);
        return;
    }

    int n = sk_read(se->fd, se->banner + se->banner_len, rem);
    if (n > 0) {
        se->banner_len += n;
        se->banner[se->banner_len] = '\0';
        if (fp_match(&g_fp_db, se->banner, se->banner_len) != FP_UNKNOWN
            || se->banner_len >= 64)
            scanner_finalize(se, SK_OPEN);
    } else if (n == 0) {
        scanner_finalize(se, SK_OPEN);
    }
}

/* ============================================================
 *  Timeout callback
 * ============================================================ */
void
scanner_on_timeout(void *arg)
{
    socket_entry_t *se = (socket_entry_t *)arg;
    if (!se || !se->in_use) return;
    se->timer_id = -1;
    scanner_finalize(se, se->state == SK_CONNECTING ? SK_TIMEOUT : SK_OPEN);
}

/* ============================================================
 *  Error — check SO_ERROR
 * ============================================================ */
void
scanner_on_error(socket_entry_t *se)
{
    int err = sk_get_error(se->fd);
    scanner_finalize(se, (err == ECONNREFUSED) ? SK_CLOSED : SK_ERROR);
}

/* ============================================================
 *  Finalize: push result, close, release, refill queue
 * ============================================================ */
void
scanner_finalize(socket_entry_t *se, sk_state_t final_state)
{
    if (!se || !se->in_use) return;
    se->state = final_state;

    if (se->timer_id > 0) {
        tw_cancel(reactor_get_timer(g_reactor), se->timer_id);
        se->timer_id = -1;
    }
    if (se->fd >= 0) reactor_del_fd(g_reactor, se->fd);

    scan_result_t r;
    memset(&r, 0, sizeof(r));
    r.target       = se->target;
    r.state        = se->state;
    r.rtt_ms       = se->rtt_ms;
    r.timestamp_ms = na_now_ms();

    if (se->banner_len > 0) {
        fp_service_t svc = fp_match(&g_fp_db, se->banner, se->banner_len);
        snprintf(r.service, sizeof(r.service), "%s", fp_service_name(svc));
        snprintf(r.banner, sizeof(r.banner), "%s", se->banner);
    } else {
        snprintf(r.service, sizeof(r.service), "unknown");
    }

    if (g_queue)    ring_push(g_queue, &r);
    if (g_callback) g_callback(&r, g_cb_ctx);

    if (se->fd >= 0) { close(se->fd); se->fd = -1; }

    /* Return slot to free-list so it can be reused */
    reactor_socket_free_entry(g_reactor, se);
    fd_mgr_release(reactor_get_fd_mgr(g_reactor));
    g_pending--;

    scanner_drain_queue();

    if (g_pending <= 0 && tq_empty() && g_reactor)
        reactor_stop(g_reactor);
}

int scanner_pending(void) { return g_pending; }
