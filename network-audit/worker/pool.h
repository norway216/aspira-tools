/*
 * pool.h — Optional pthread Worker Pool
 */
#ifndef WORKER_POOL_H
#define WORKER_POOL_H

#include "../include/net_audit.h"

/* ---- Lifecycle ---- */
int  pool_init(worker_pool_t *pool, int thread_count,
               ring_queue_t *in, ring_queue_t *out);
void pool_start(worker_pool_t *pool);
void pool_stop(worker_pool_t *pool);
void pool_cleanup(worker_pool_t *pool);

/* ---- Submit (thread-safe, called from reactor thread) ---- */
int  pool_submit(worker_pool_t *pool, const worker_task_t *task);

/* ---- Query ---- */
int  pool_active(worker_pool_t *pool);

#endif /* WORKER_POOL_H */
