/*
 * timer_wheel.h — O(1) Timer Management
 */
#ifndef CORE_TIMER_WHEEL_H
#define CORE_TIMER_WHEEL_H

#include "../include/net_audit.h"

/* ---- Lifecycle ---- */
int  tw_init(timer_wheel_t *tw, int max_nodes);
void tw_cleanup(timer_wheel_t *tw);

/* ---- Operations ---- */
int  tw_add(timer_wheel_t *tw, int delay_ticks, timer_cb_t cb, void *arg);
int  tw_cancel(timer_wheel_t *tw, int timer_id);
void tw_tick(timer_wheel_t *tw);        /* advance one tick, fire expired */
int  tw_count(timer_wheel_t *tw);

/* Get current tick (for RTT calculation) */
int  tw_current_tick(timer_wheel_t *tw);

#endif /* CORE_TIMER_WHEEL_H */
