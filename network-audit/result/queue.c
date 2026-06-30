/*
 * queue.c — Lock-Free SPSC Ring Buffer (C11 atomics)
 *
 * Single-producer (reactor thread), single-consumer (main thread).
 * Uses C11 atomic_thread_fence for portability (Fix #9).
 * One slot is always kept empty → effective capacity = NA_RING_SIZE - 1.
 */
#include "queue.h"
#include <stdatomic.h>

/* ---- Lifecycle ---- */

void
ring_init(ring_queue_t *q)
{
    q->head = 0;
    q->tail = 0;
}

/* ---- Producer ---- */

int
ring_push(ring_queue_t *q, const scan_result_t *item)
{
    int next = (q->head + 1) % NA_RING_SIZE;

    if (next == q->tail) {
        return -1;   /* full */
    }

    q->items[q->head] = *item;

    /* C11 full memory barrier — portable across x86, ARM, RISC-V */
    atomic_thread_fence(memory_order_seq_cst);

    q->head = next;
    return 0;
}

/* ---- Consumer ---- */

int
ring_pop(ring_queue_t *q, scan_result_t *item)
{
    if (q->tail == q->head) {
        return -1;   /* empty */
    }

    *item = q->items[q->tail];

    atomic_thread_fence(memory_order_seq_cst);

    q->tail = (q->tail + 1) % NA_RING_SIZE;
    return 0;
}

/* ---- Query ---- */

int
ring_count(ring_queue_t *q)
{
    if (q->head >= q->tail) {
        return q->head - q->tail;
    }
    return NA_RING_SIZE - q->tail + q->head;
}

int
ring_is_empty(ring_queue_t *q)
{
    return q->tail == q->head;
}

int
ring_is_full(ring_queue_t *q)
{
    return ((q->head + 1) % NA_RING_SIZE) == q->tail;
}
