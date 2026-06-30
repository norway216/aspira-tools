/*
 * pool.c — Optional pthread Worker Pool
 *
 * Workers handle CPU-heavy tasks off the reactor thread:
 *   - TASK_RISK_SCORE: compute a simple risk heuristic
 *
 * Note: TASK_FP_PARSE and TASK_JSON_SERIALIZE are handled inline
 * in the reactor/main thread for efficiency. The worker pool is
 * designed for optional offloading of risk scoring on large scans.
 *
 * Default: 0 threads (all work done inline in reactor thread).
 */

#include "pool.h"
#include "../result/queue.h"
#include <stdlib.h>
#include <stdio.h>

/* ============================================================
 *  Internal: simple risk scoring heuristic
 *
 *  Returns 0-100. Higher = more interesting from a security
 *  perspective (open uncommon ports, known risky services, etc.).
 * ============================================================ */

static int
compute_risk_score(const scan_result_t *r)
{
    int score = 0;

    if (r->state == SK_OPEN)     score += 10;
    uint16_t port = na_port_host(r->target.port);
    if (port > 1024)             score += 5;
    if (strstr(r->service, "SSH"))    score += 15;
    if (strstr(r->service, "Telnet")) score += 30;
    if (strstr(r->service, "FTP"))    score += 20;
    if (strstr(r->service, "MySQL"))  score += 10;
    if (strstr(r->service, "RDP"))    score += 15;
    if (r->banner[0] != '\0')    score += 5;
    if (score > 100) score = 100;
    if (score < 0)   score = 0;
    return score;
}

/* ============================================================
 *  Internal: worker thread function
 * ============================================================ */

static void *
worker_thread_func(void *arg)
{
    worker_pool_t *pool = (worker_pool_t *)arg;

    while (pool->running) {
        worker_task_t task;

        pthread_mutex_lock(&pool->mutex);

        while (pool->running && ring_is_empty(pool->input_queue)) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }

        if (!pool->running) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        int ok = ring_pop(pool->input_queue, (scan_result_t *)&task.result);
        pthread_mutex_unlock(&pool->mutex);

        if (ok < 0) continue;

        /* Process based on task type */
        switch (task.type) {
        case TASK_RISK_SCORE: {
            int risk = compute_risk_score(&task.result);
            task.result.rtt_ms = risk;
            break;
        }
        case TASK_FP_PARSE:
        case TASK_JSON_SERIALIZE:
        default:
            break;
        }

        if (pool->output_queue) {
            ring_push(pool->output_queue, &task.result);
        }
    }

    return NULL;
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

int
pool_init(worker_pool_t *pool, int thread_count,
          ring_queue_t *in, ring_queue_t *out)
{
    memset(pool, 0, sizeof(*pool));

    if (thread_count <= 0) {
        return 0;
    }
    if (thread_count > NA_WORKER_MAX) {
        thread_count = NA_WORKER_MAX;
    }

    pool->threads = (pthread_t *)calloc((size_t)thread_count, sizeof(pthread_t));
    if (!pool->threads) {
        return -1;
    }

    pool->thread_count = thread_count;
    pool->input_queue  = in;
    pool->output_queue = out;
    pool->running      = 0;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    return 0;
}

void
pool_start(worker_pool_t *pool)
{
    if (pool->thread_count <= 0) return;

    pool->running = 1;

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_create(&pool->threads[i], NULL, worker_thread_func, pool);
    }
}

void
pool_stop(worker_pool_t *pool)
{
    if (pool->thread_count <= 0) return;

    pool->running = 0;

    pthread_mutex_lock(&pool->mutex);
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
}

void
pool_cleanup(worker_pool_t *pool)
{
    if (pool->threads) {
        free(pool->threads);
        pool->threads = NULL;
    }
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    pool->thread_count = 0;
}

/* ============================================================
 *  Submit task
 * ============================================================ */

int
pool_submit(worker_pool_t *pool, const worker_task_t *task)
{
    if (pool->thread_count <= 0 || !pool->running) {
        return -1;
    }

    pthread_mutex_lock(&pool->mutex);

    int ret = ring_push(pool->input_queue, &task->result);

    if (ret == 0) {
        pthread_cond_signal(&pool->cond);
    }

    pthread_mutex_unlock(&pool->mutex);
    return ret;
}

/* ============================================================
 *  Query
 * ============================================================ */

int
pool_active(worker_pool_t *pool)
{
    return pool->running ? pool->thread_count : 0;
}
