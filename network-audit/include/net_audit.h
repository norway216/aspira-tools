/*
 * net_audit.h — Master Header for Lightweight Network Audit Framework V1
 *
 * Defines all shared types, enums, constants, and inline helpers used across
 * every module. This header must be self-contained and should be included
 * first in every .c file.
 *
 * Design constraints:
 *   - C11 standard (Linux-only, requires epoll and SOCK_NONBLOCK)
 *   - <20MB runtime memory (pre-allocated, zero hot-path malloc)
 *   - Single reactor thread + optional worker pool
 */
#ifndef NET_AUDIT_H
#define NET_AUDIT_H

/* Feature test macro: required for clock_gettime, SA_RESTART, etc. */
#if !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

/* ---- System headers (every module needs these) ---- */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ============================================================
 *  Feature toggles
 * ============================================================ */

/* HAVE_SQLITE3 is set via Makefile (-DHAVE_SQLITE3) when sqlite3-dev is present */

/* ============================================================
 *  Limits & Constants
 * ============================================================ */

#define NA_MAX_TARGETS       65536    /* max IP:port pairs in one scan   */
#define NA_PORT_MAX          65535
#define NA_IP_STR_LEN        48       /* enough for IPv4 + IPv6 mapped  */
#define NA_SERVICE_STR_LEN   64
#define NA_BANNER_LEN        128      /* truncated banner for FP match  */
#define NA_CONCURRENCY_MAX   50000    /* absolute upper bound on FDs    */
#define NA_CONCURRENCY_DEF   1000
#define NA_TIMEOUT_DEF_MS    3000
#define NA_EVENT_BATCH       64       /* epoll_wait batch size          */
#define NA_WHEEL_SIZE        256      /* timer wheel slots              */
#define NA_TICK_MS           10       /* timer resolution (milliseconds) */
#define NA_RING_SIZE         1024     /* result ring buffer capacity    */
#define NA_WORKER_MAX        16
#define NA_FP_PATTERNS_MAX   64
#define NA_JSON_BUF_SIZE     4096
#define NA_ADDR_STR_LEN      256
/* ============================================================
 *  Socket / Connection State Machine
 * ============================================================ */

typedef enum {
    SK_INIT       = 0,
    SK_CONNECTING = 1,
    SK_OPEN       = 2,
    SK_CLOSED     = 3,
    SK_TIMEOUT    = 4,
    SK_ERROR      = 5
} sk_state_t;

/* Human-readable label for logging */
static inline const char *
sk_state_label(sk_state_t s)
{
    switch (s) {
    case SK_INIT:       return "INIT";
    case SK_CONNECTING: return "CONNECTING";
    case SK_OPEN:       return "OPEN";
    case SK_CLOSED:     return "CLOSED";
    case SK_TIMEOUT:    return "TIMEOUT";
    case SK_ERROR:      return "ERROR";
    default:            return "UNKNOWN";
    }
}

/* ============================================================
 *  Scan Target / Result
 * ============================================================ */

/* Compact 6-byte target: IPv4 + port, network byte order */
typedef struct {
    uint32_t ip;       /* network byte order (from inet_addr / htonl) */
    uint16_t port;     /* network byte order (htons)                  */
} __attribute__((packed)) scan_target_t;

/* Result pushed into ring buffer after each completed scan */
typedef struct {
    scan_target_t  target;
    sk_state_t     state;
    int            rtt_ms;              /* -1 = not measured          */
    char           service[NA_SERVICE_STR_LEN];
    char           banner[NA_BANNER_LEN];
    int64_t        timestamp_ms;        /* monotonic time when done   */
} scan_result_t;

/* ============================================================
 *  Timer Wheel
 * ============================================================ */

struct timer_node;                      /* forward decl for typedef   */
typedef void (*timer_cb_t)(void *arg);

typedef struct timer_node {
    int            remaining_ticks;     /* for multi-wrap support     */
    timer_cb_t     callback;
    void          *arg;
    struct timer_node *next;            /* collision chain in slot    */
    int            id;                  /* unique id for cancellation */
} timer_node_t;

/* Pre-allocated pool — zero malloc in hot path */
typedef struct {
    timer_node_t  *nodes;               /* array [NA_TMR_POOL_SIZE]   */
    int           *free_stack;          /* indices of free nodes      */
    int            free_top;            /* stack pointer              */
    int            capacity;
} timer_pool_t;

/* O(1) timer wheel */
typedef struct {
    timer_node_t **slots;               /* array [NA_WHEEL_SIZE]      */
    timer_pool_t   pool;
    int            current_tick;        /* monotonic, wraps           */
    int            next_id;             /* monotonically increasing   */
    int            total;               /* active timer count         */
} timer_wheel_t;

/* ============================================================
 *  FD Manager
 * ============================================================ */

typedef struct {
    int fd_limit;                       /* configured max (--concurrency) */
    int current_fd;                     /* live socket count              */
} fd_manager_t;

/* ============================================================
 *  Per-Socket Entry (hot-path data, pre-allocated array)
 *
 *  Each entry is ~152 bytes. At 50K concurrency: ~7.6 MB.
 *  Well within the <20MB target.
 * ============================================================ */

typedef struct {
    int64_t        start_tick;          /* 8 bytes, largest first      */
    int            fd;                  /* socket file descriptor      */
    int            rtt_ms;              /* -1 = not measured           */
    int            banner_len;          /* bytes read so far           */
    int            timer_id;            /* -1 = no active timer        */
    int            in_use;              /* 0 = free, 1 = occupied      */
    sk_state_t     state;               /* current state machine state */
    scan_target_t  target;              /* 6 bytes packed              */
    char           banner[NA_BANNER_LEN]; /* read buffer + null term   */
} socket_entry_t;

/* ============================================================
 *  Reactor (epoll event loop)
 * ============================================================ */

typedef void (*reactor_cb_t)(struct epoll_event *ev, void *ctx);

typedef struct {
    int             epoll_fd;
    volatile int    running;
    fd_manager_t    fd_mgr;
    timer_wheel_t   timer;
    socket_entry_t *sockets;            /* array [socket_capacity]     */
    int            *free_list;          /* O(1) free-slot stack        */
    int             free_top;           /* stack pointer               */
    int             socket_capacity;
} reactor_t;

/* ============================================================
 *  Fingerprint DB
 * ============================================================ */

typedef enum {
    FP_UNKNOWN  = 0,
    FP_SSH      = 1,
    FP_HTTP     = 2,
    FP_HTTPS    = 3,
    FP_TLS      = 4,
    FP_SMTP     = 5,
    FP_FTP      = 6,
    FP_MYSQL    = 7,
    FP_RDP      = 8,
    FP_DNS      = 9,
    FP_TELNET   = 10,
    FP_RTSP     = 11,
    FP_IMAP     = 12,
    FP_POP3     = 13
} fp_service_t;

typedef struct {
    const char   *pattern;              /* substring to match         */
    fp_service_t  service;
    const char   *service_name;         /* "SSH", "HTTP", etc.        */
    int           min_match_len;        /* min banner bytes needed    */
} fp_entry_t;

typedef struct {
    fp_entry_t entries[NA_FP_PATTERNS_MAX];
    int        count;
} fp_db_t;

/* ============================================================
 *  Lock-Free Ring Queue (SPSC)
 *
 *  Single producer (reactor thread), single consumer (main thread).
 *  One slot is always kept empty to distinguish full from empty.
 *  Effective capacity: NA_RING_SIZE - 1 = 1023 items.
 * ============================================================ */

typedef struct {
    scan_result_t items[NA_RING_SIZE];
    volatile int  head;                 /* producer writes here       */
    volatile int  tail;                 /* consumer reads here        */
} ring_queue_t;

/* ============================================================
 *  Worker Pool
 * ============================================================ */

typedef enum {
    TASK_FP_PARSE       = 0,
    TASK_JSON_SERIALIZE = 1,
    TASK_RISK_SCORE     = 2
} task_type_t;

typedef struct {
    task_type_t   type;
    scan_result_t result;               /* copy (small, fixed size)   */
} worker_task_t;

typedef struct {
    pthread_t      *threads;
    int             thread_count;
    ring_queue_t   *input_queue;        /* reactor → workers          */
    ring_queue_t   *output_queue;       /* workers → main thread      */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    volatile int    running;
} worker_pool_t;

/* ============================================================
 *  SQLite Storage (optional — compiled out when !HAVE_SQLITE3)
 * ============================================================ */

typedef struct {
    char     ip[NA_IP_STR_LEN];
    uint16_t port;
    char     service[NA_SERVICE_STR_LEN];
    int      state;                     /* sk_state_t cast to int     */
    int64_t  last_seen;                 /* unix epoch seconds         */
} asset_row_t;

/* ============================================================
 *  CLI Configuration
 * ============================================================ */

typedef enum {
    OUTPUT_TEXT = 0,
    OUTPUT_JSON = 1
} output_format_t;

typedef struct {
    /* Target specification */
    char     target_arg[256];           /* raw --target argument      */
    int      port_start;
    int      port_end;
    /* Runtime parameters */
    int      concurrency;
    int      timeout_ms;
    int      worker_threads;
    output_format_t output_fmt;
    /* Optional persistence */
    int      use_db;
    char     db_path[256];
} na_config_t;

/* ============================================================
 *  Global shutdown flag (set by signal handlers, C11 atomic)
 * ============================================================ */

extern atomic_int g_shutdown;

/* ============================================================
 *  Inline helpers
 * ============================================================ */

/* Get monotonic time in milliseconds */
static inline int64_t
na_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* IP uint32_t (network byte order) → dotted-decimal string */
static inline const char *
na_ip_to_str(uint32_t ip, char *buf, size_t buflen)
{
    struct in_addr in;
    in.s_addr = ip;
    const char *p = inet_ntop(AF_INET, &in, buf, (socklen_t)buflen);
    return p ? p : "?.?.?.?";
}

/* Convert port from network to host byte order */
static inline uint16_t
na_port_host(uint16_t port_net)
{
    return ntohs(port_net);
}

/* Derive timer ticks from milliseconds */
static inline int
na_ms_to_ticks(int ms)
{
    return (ms + NA_TICK_MS - 1) / NA_TICK_MS;  /* round up */
}

#endif /* NET_AUDIT_H */
