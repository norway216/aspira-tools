/*
 * queue.h — Lock-Free SPSC Ring Buffer
 */
#ifndef RESULT_QUEUE_H
#define RESULT_QUEUE_H

#include "../include/net_audit.h"

/* ---- Lifecycle ---- */
void ring_init(ring_queue_t *q);

/* ---- Producer (reactor thread) ---- */
int  ring_push(ring_queue_t *q, const scan_result_t *item);

/* ---- Consumer (main thread) ---- */
int  ring_pop(ring_queue_t *q, scan_result_t *item);

/* ---- Query ---- */
int  ring_count(ring_queue_t *q);
int  ring_is_empty(ring_queue_t *q);
int  ring_is_full(ring_queue_t *q);

#endif /* RESULT_QUEUE_H */
