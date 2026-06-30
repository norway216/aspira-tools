/*
 * scanner.h — TCP Connect Scan Engine (State Machine)
 */
#ifndef SCAN_SCANNER_H
#define SCAN_SCANNER_H

#include "../include/net_audit.h"

/* Result callback: called from reactor thread for each completed scan */
typedef void (*scan_callback_t)(const scan_result_t *result, void *ctx);

/* ---- Lifecycle ---- */
int  scanner_init(reactor_t *r, ring_queue_t *queue,
                  scan_callback_t cb, void *ctx);
void scanner_cleanup(void);

/* ---- Target submission ---- */
int  scanner_submit_targets(const scan_target_t *targets, int count);
void scanner_set_timeout_ms(int ms);

/* ---- Event handler (registered with reactor) ---- */
void scanner_on_event(struct epoll_event *ev, void *ctx);

/* ---- State machine transitions (called internally) ---- */
void scanner_on_connect_done(socket_entry_t *se);
void scanner_on_readable(socket_entry_t *se);
void scanner_on_timeout(void *arg);
void scanner_on_error(socket_entry_t *se);
void scanner_finalize(socket_entry_t *se, sk_state_t final_state);

/* ---- Status ---- */
int  scanner_pending(void);            /* active + inflight count    */

#endif /* SCAN_SCANNER_H */
