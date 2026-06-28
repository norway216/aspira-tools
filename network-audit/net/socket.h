/*
 * socket.h — Non-blocking Socket Operations
 */
#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include "../include/net_audit.h"

/* ---- Socket lifecycle ---- */
int sk_create(int *fd_out);
int sk_close(int *fd);

/* ---- Non-blocking I/O ---- */
int sk_set_nonblock(int fd);
int sk_set_cloexec(int fd);
int sk_set_nodelay(int fd);            /* TCP_NODELAY for low-latency */

/* ---- Connection ---- */
int sk_connect_nonblock(int fd, uint32_t ip, uint16_t port);
int sk_get_error(int fd);              /* getsockopt SO_ERROR */

/* ---- Read ---- */
int sk_read(int fd, char *buf, size_t len);

/* ---- FD Manager ---- */
void fd_mgr_init(fd_manager_t *mgr, int limit);
int  fd_mgr_acquire(fd_manager_t *mgr);
void fd_mgr_release(fd_manager_t *mgr);
int  fd_mgr_available(fd_manager_t *mgr);

#endif /* NET_SOCKET_H */
