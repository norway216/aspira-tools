/*
 * socket.c — Non-blocking Socket Operations + FD Manager
 *
 * Thin wrappers around POSIX socket API. All sockets are created with
 * SOCK_NONBLOCK | SOCK_CLOEXEC (Linux 2.6.27+) to avoid extra syscalls.
 */
#include "socket.h"

/* ============================================================
 *  Socket lifecycle
 * ============================================================ */

int
sk_create(int *fd_out)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    *fd_out = fd;
    return 0;
}

int
sk_close(int *fd)
{
    if (!fd || *fd < 0) {
        return -1;
    }
    close(*fd);
    *fd = -1;
    return 0;
}

/* ============================================================
 *  Socket options
 * ============================================================ */

int
sk_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int
sk_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int
sk_set_nodelay(int fd)
{
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

/* ============================================================
 *  Non-blocking connect
 * ============================================================ */

int
sk_connect_nonblock(int fd, uint32_t ip, uint16_t port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = ip;
    addr.sin_port        = port;

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));

    if (ret == 0) {
        /* Immediate success (rare: same-host or already-established) */
        return 0;
    }

    if (errno == EINPROGRESS) {
        /* Expected: connection is in progress */
        return 0;
    }

    /* Genuine error (e.g., ENETUNREACH, ECONNREFUSED) */
    return -1;
}

/* ============================================================
 *  Socket error check
 * ============================================================ */

int
sk_get_error(int fd)
{
    int       err = 0;
    socklen_t len = sizeof(err);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno;
    }
    return err;
}

/* ============================================================
 *  Non-blocking read
 * ============================================================ */

int
sk_read(int fd, char *buf, size_t len)
{
    ssize_t n = read(fd, buf, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;                  /* no data available yet */
        }
        if (errno == EINTR) {
            return 0;                  /* interrupted, caller will retry */
        }
        return -1;                     /* real error */
    }
    return (int)n;                     /* 0 = EOF, >0 = bytes read */
}

/* ============================================================
 *  FD Manager
 * ============================================================ */

void
fd_mgr_init(fd_manager_t *mgr, int limit)
{
    mgr->fd_limit   = limit;
    mgr->current_fd = 0;
}

int
fd_mgr_acquire(fd_manager_t *mgr)
{
    if (mgr->current_fd >= mgr->fd_limit) {
        return -1;                     /* at capacity */
    }
    mgr->current_fd++;
    return 0;
}

void
fd_mgr_release(fd_manager_t *mgr)
{
    if (mgr->current_fd > 0) {
        mgr->current_fd--;
    }
}

int
fd_mgr_available(fd_manager_t *mgr)
{
    return mgr->fd_limit - mgr->current_fd;
}
