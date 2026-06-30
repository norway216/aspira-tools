/*
 * reactor.c — epoll Event Loop with Integrated FD Manager and Timer Wheel
 *
 * This is the backbone of the framework. All I/O flows through the reactor:
 *   - socket lifecycle (add/mod/del fd to/from epoll)
 *   - timer wheel tick advancement (every NA_TICK_MS)
 *   - FD manager enforcement (concurrency limit)
 *   - O(1) socket entry allocation via free-list
 *
 * Design: single-threaded event loop. No locks needed inside the reactor.
 */

#include "reactor.h"
#include "timer_wheel.h"
#include "../net/socket.h"
#include <stdlib.h>
#include <string.h>

/* ---- Global shutdown flag (set by signal handler in main.c) ---- */
atomic_int g_shutdown = 0;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

int
reactor_init(reactor_t *r, int max_fd)
{
    memset(r, 0, sizeof(*r));

    if (max_fd > NA_CONCURRENCY_MAX) {
        max_fd = NA_CONCURRENCY_MAX;
    }
    if (max_fd < 1) {
        max_fd = NA_CONCURRENCY_DEF;
    }

    r->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epoll_fd < 0) {
        return -1;
    }

    /* Allocate socket entry array */
    r->sockets = (socket_entry_t *)calloc((size_t)max_fd, sizeof(socket_entry_t));
    if (!r->sockets) {
        close(r->epoll_fd);
        r->epoll_fd = -1;
        return -1;
    }
    r->socket_capacity = max_fd;

    /*
     * Free-list for O(1) socket entry allocation (Fix #8).
     * Stack grows downward: free_stack[free_top-1] is the next free index.
     */
    r->free_list = (int *)calloc((size_t)max_fd, sizeof(int));
    if (!r->free_list) {
        free(r->sockets);
        close(r->epoll_fd);
        r->sockets = NULL;
        r->epoll_fd = -1;
        return -1;
    }
    /* Push all indices (reverse order so lower indices are popped first) */
    for (int i = max_fd - 1; i >= 0; i--) {
        r->free_list[r->free_top++] = i;
    }

    /* Initialize FD manager */
    fd_mgr_init(&r->fd_mgr, max_fd);

    /* Initialize timer wheel */
    if (tw_init(&r->timer, max_fd) < 0) {
        free(r->free_list);
        free(r->sockets);
        close(r->epoll_fd);
        r->free_list = NULL;
        r->sockets = NULL;
        r->epoll_fd = -1;
        return -1;
    }

    r->running = 0;
    return 0;
}

void
reactor_cleanup(reactor_t *r)
{
    if (!r) return;

    if (r->sockets) {
        for (int i = 0; i < r->socket_capacity; i++) {
            if (r->sockets[i].in_use && r->sockets[i].fd >= 0) {
                close(r->sockets[i].fd);
                r->sockets[i].fd = -1;
            }
        }
        free(r->sockets);
        r->sockets = NULL;
    }

    free(r->free_list);
    r->free_list = NULL;

    tw_cleanup(&r->timer);

    if (r->epoll_fd >= 0) {
        close(r->epoll_fd);
        r->epoll_fd = -1;
    }

    r->socket_capacity = 0;
}

/* ============================================================
 *  FD-to-epoll mapping
 * ============================================================ */

int
reactor_add_fd(reactor_t *r, int fd, uint32_t events, socket_entry_t *entry)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = events | EPOLLET;
    ev.data.ptr = entry;

    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return -1;
    }
    return 0;
}

int
reactor_mod_fd(reactor_t *r, int fd, uint32_t events, socket_entry_t *entry)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = events | EPOLLET;
    ev.data.ptr = entry;

    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        return -1;
    }
    return 0;
}

int
reactor_del_fd(reactor_t *r, int fd)
{
    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        if (errno != ENOENT) return -1;
    }
    return 0;
}

/* ============================================================
 *  Socket entry array (O(1) alloc/free via free-list — Fix #8)
 * ============================================================ */

socket_entry_t *
reactor_socket_alloc(reactor_t *r)
{
    if (r->free_top <= 0) {
        return NULL;   /* no free slots */
    }
    int idx = r->free_list[--r->free_top];
    memset(&r->sockets[idx], 0, sizeof(socket_entry_t));
    r->sockets[idx].in_use   = 1;
    r->sockets[idx].fd       = -1;
    r->sockets[idx].timer_id = -1;
    r->sockets[idx].rtt_ms   = -1;
    return &r->sockets[idx];
}

void
reactor_socket_free_entry(reactor_t *r, socket_entry_t *se)
{
    if (!se || !r) return;

    if (se->fd >= 0) {
        reactor_del_fd(r, se->fd);
        close(se->fd);
    }
    memset(se, 0, sizeof(*se));
    se->fd     = -1;
    se->in_use = 0;

    /* Return index to free-list */
    ptrdiff_t idx = se - r->sockets;
    if (idx >= 0 && idx < r->socket_capacity) {
        r->free_list[r->free_top++] = (int)idx;
    }
}

int
reactor_socket_index(reactor_t *r, socket_entry_t *se)
{
    if (!r || !se) return -1;
    ptrdiff_t idx = se - r->sockets;
    if (idx < 0 || idx >= r->socket_capacity) return -1;
    return (int)idx;
}

/* ============================================================
 *  Event loop
 * ============================================================ */

int
reactor_run(reactor_t *r, reactor_cb_t cb, void *ctx)
{
    struct epoll_event events[NA_EVENT_BATCH];

    r->running = 1;

    while (r->running && !atomic_load(&g_shutdown)) {
        int nfds = epoll_wait(r->epoll_fd, events, NA_EVENT_BATCH, NA_TICK_MS);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        for (int i = 0; i < nfds; i++) {
            if (cb) {
                cb(&events[i], ctx);
            }
        }

        tw_tick(&r->timer);
    }

    return 0;
}

void
reactor_stop(reactor_t *r)
{
    r->running = 0;
}

/* ============================================================
 *  Accessors
 * ============================================================ */

fd_manager_t *
reactor_get_fd_mgr(reactor_t *r)
{
    return &r->fd_mgr;
}

timer_wheel_t *
reactor_get_timer(reactor_t *r)
{
    return &r->timer;
}
