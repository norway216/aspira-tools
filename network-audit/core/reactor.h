/*
 * reactor.h — epoll Event Loop + FD Manager
 */
#ifndef CORE_REACTOR_H
#define CORE_REACTOR_H

#include "../include/net_audit.h"

/* ---- Lifecycle ---- */
int  reactor_init(reactor_t *r, int max_fd);
void reactor_cleanup(reactor_t *r);

/* ---- FD-to-epoll mapping ---- */
int  reactor_add_fd(reactor_t *r, int fd, uint32_t events, socket_entry_t *entry);
int  reactor_mod_fd(reactor_t *r, int fd, uint32_t events, socket_entry_t *entry);
int  reactor_del_fd(reactor_t *r, int fd);

/* ---- Socket entry array (O(1) alloc/free via free-list) ---- */
socket_entry_t *reactor_socket_alloc(reactor_t *r);
void            reactor_socket_free_entry(reactor_t *r, socket_entry_t *se);
int             reactor_socket_index(reactor_t *r, socket_entry_t *se);

/* ---- Event loop ---- */
int  reactor_run(reactor_t *r, reactor_cb_t cb, void *ctx);
void reactor_stop(reactor_t *r);

/* ---- Accessors ---- */
fd_manager_t  *reactor_get_fd_mgr(reactor_t *r);
timer_wheel_t *reactor_get_timer(reactor_t *r);

#endif /* CORE_REACTOR_H */
