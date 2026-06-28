/*
 * queue.c — Lock-Free SPSC Ring Buffer
 *
 * Single-producer (reactor thread), single-consumer (main thread).
 * Uses memory barriers instead of locks for zero contention in hot path.
 * One slot is always kept empty → effective capacity = NA_RING_SIZE - 1.
 */
#include "queue.h"

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

    /* Full: next would overwrite tail (kept-empty slot) */
    if (next == q->tail) {
        return -1;
    }

    q->items[q->head] = *item;

    /*
     * Full memory barrier: ensure the item copy is visible before
     * the head pointer advances. On x86 this is an mfence; on ARM
     * it's a dmb. Required for correctness on weakly-ordered CPUs.
     */
    __sync_synchronize();

    q->head = next;
    return 0;
}

/* ---- Consumer ---- */

int
ring_pop(ring_queue_t *q, scan_result_t *item)
{
    /* Empty */
    if (q->tail == q->head) {
        return -1;
    }

    *item = q->items[q->tail];

    __sync_synchronize();

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
