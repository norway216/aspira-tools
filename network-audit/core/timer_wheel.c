/*
 * timer_wheel.c — O(1) Timer Wheel with Pre-allocated Node Pool
 *
 * Architecture:
 *   - 256-slot hash-wheel (WHEEL_SIZE). Each slot is a singly-linked list.
 *   - Nodes carry remaining_ticks for multi-wrap support. On each tick,
 *     the current slot is traversed; nodes with remaining_ticks > 0 are
 *     re-slotted; nodes with remaining_ticks == 0 are fired and freed.
 *   - Pre-allocated node pool uses an internal free-stack for O(1) alloc/free.
 *     Zero malloc in the hot path.
 *
 * Timer resolution: NA_TICK_MS = 10ms.
 * Max timer duration per wrap: 256 * 10ms = 2.56s.
 * Longer durations supported via remaining_ticks wrapping.
 */

#include "timer_wheel.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================
 *  Internal: node pool alloc/free (O(1))
 * ============================================================ */

static timer_node_t *
pool_alloc(timer_pool_t *p)
{
    if (p->free_top <= 0) {
        return NULL;                    /* pool exhausted */
    }
    int idx = p->free_stack[--p->free_top];
    memset(&p->nodes[idx], 0, sizeof(timer_node_t));
    return &p->nodes[idx];
}

static void
pool_free(timer_pool_t *p, timer_node_t *node)
{
    int idx = (int)(node - p->nodes);
    p->free_stack[p->free_top++] = idx;
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

int
tw_init(timer_wheel_t *tw, int max_nodes)
{
    memset(tw, 0, sizeof(*tw));

    /* Allocate slots array */
    tw->slots = (timer_node_t **)calloc((size_t)NA_WHEEL_SIZE, sizeof(timer_node_t *));
    if (!tw->slots) {
        return -1;
    }

    /* Allocate node pool */
    tw->pool.nodes = (timer_node_t *)calloc((size_t)max_nodes, sizeof(timer_node_t));
    if (!tw->pool.nodes) {
        free(tw->slots);
        tw->slots = NULL;
        return -1;
    }

    /* Allocate free stack */
    tw->pool.free_stack = (int *)calloc((size_t)max_nodes, sizeof(int));
    if (!tw->pool.free_stack) {
        free(tw->pool.nodes);
        free(tw->slots);
        tw->pool.nodes = NULL;
        tw->slots = NULL;
        return -1;
    }

    /* Initialize free stack with all indices */
    tw->pool.capacity = max_nodes;
    tw->pool.free_top = max_nodes;
    for (int i = 0; i < max_nodes; i++) {
        tw->pool.free_stack[i] = i;
    }

    tw->current_tick = 0;
    tw->next_id = 1;
    tw->total = 0;

    return 0;
}

void
tw_cleanup(timer_wheel_t *tw)
{
    free(tw->pool.free_stack);
    free(tw->pool.nodes);
    free(tw->slots);
    memset(tw, 0, sizeof(*tw));
}

/* ============================================================
 *  Add timer
 * ============================================================ */

int
tw_add(timer_wheel_t *tw, int delay_ticks, timer_cb_t cb, void *arg)
{
    if (delay_ticks <= 0) {
        delay_ticks = 1;               /* minimum 1 tick */
    }

    timer_node_t *node = pool_alloc(&tw->pool);
    if (!node) {
        return -1;                      /* pool exhausted */
    }

    node->remaining_ticks = delay_ticks;
    node->callback        = cb;
    node->arg             = arg;
    node->id              = tw->next_id++;

    int slot = (tw->current_tick + delay_ticks) % NA_WHEEL_SIZE;
    node->next = tw->slots[slot];
    tw->slots[slot] = node;
    tw->total++;

    return node->id;
}

/* ============================================================
 *  Cancel timer
 * ============================================================ */

int
tw_cancel(timer_wheel_t *tw, int timer_id)
{
    if (timer_id <= 0 || tw->total == 0) {
        return -1;
    }

    /*
     * Search all slots. In practice, cancelled timers are rare and
     * the collision chains are short, so linear scan is acceptable.
     */
    for (int i = 0; i < NA_WHEEL_SIZE; i++) {
        timer_node_t *prev = NULL;
        timer_node_t *curr = tw->slots[i];

        while (curr) {
            if (curr->id == timer_id) {
                /* Unlink */
                if (prev) {
                    prev->next = curr->next;
                } else {
                    tw->slots[i] = curr->next;
                }
                pool_free(&tw->pool, curr);
                tw->total--;
                return 0;
            }
            prev = curr;
            curr = curr->next;
        }
    }
    return -1;                          /* not found */
}

/* ============================================================
 *  Advance one tick — fire expired timers
 * ============================================================ */

void
tw_tick(timer_wheel_t *tw)
{
    int slot = tw->current_tick % NA_WHEEL_SIZE;
    timer_node_t *prev = NULL;
    timer_node_t *curr = tw->slots[slot];
    timer_node_t *next;

    while (curr) {
        next = curr->next;

        curr->remaining_ticks--;

        if (curr->remaining_ticks <= 0) {
            /*
             * Save old slot head before callback — the callback may
             * add new timers to this same slot via tw_add(), which
             * prepends to tw->slots[slot].  We must preserve those
             * new nodes.  Fix #2.
             */
            timer_node_t *saved_head = tw->slots[slot];

            if (curr->callback) {
                curr->callback(curr->arg);
            }

            /* Unlink from current slot */
            if (prev) {
                prev->next = next;
            } else {
                tw->slots[slot] = next;
            }

            /*
             * If callback added new timers to this slot,
             * saved_head != tw->slots[slot] before we overwrote it.
             * Merge: append our remaining chain (next) to the
             * tail of the newly-added nodes, then restore the
             * merged chain as the slot head.
             */
            if (saved_head != tw->slots[slot] && saved_head != curr) {
                timer_node_t *walk = saved_head;
                while (walk->next && walk->next != curr) {
                    walk = walk->next;
                }
                walk->next = next;
                tw->slots[slot] = saved_head;
            }

            pool_free(&tw->pool, curr);
            tw->total--;
        } else {
            /*
             * Re-slot: move to the correct future slot.
             */
            int new_slot = (tw->current_tick + 1
                            + curr->remaining_ticks) % NA_WHEEL_SIZE;

            if (prev) {
                prev->next = next;
            } else {
                tw->slots[slot] = next;
            }

            /* Insert at head of new slot */
            curr->next = tw->slots[new_slot];
            tw->slots[new_slot] = curr;
        }

        curr = next;
    }

    tw->current_tick++;
}

/* ============================================================
 *  Query
 * ============================================================ */

int
tw_count(timer_wheel_t *tw)
{
    return tw->total;
}

int
tw_current_tick(timer_wheel_t *tw)
{
    return tw->current_tick;
}
