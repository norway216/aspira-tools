#include "event_bus.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/*
 * Handle encoding: upper 24 bits = generation counter, lower 8 bits = slot index + 1.
 * This prevents stale-handle bugs after unsubscribe/re-subscribe cycles.
 */
#define HANDLE_GEN_SHIFT   8
#define HANDLE_SLOT_MASK   0xFF
#define MAX_GENERATION     0x00FFFFFF

typedef struct {
    event_subscriber_t handle;       /* encoded gen+slot */
    uint32_t           generation;   /* current gen for this slot */
    event_type_t       type;
    event_handler_t    handler;
    void              *user_data;
    bool               active;
} subscriber_entry_t;

static subscriber_entry_t subscribers[EVENT_BUS_MAX_SUBSCRIBERS];
static uint32_t           global_gen = 0;   /* increments on each new subscription */
static bool               publishing = false; /* guard against re-entrant publish */
static pthread_mutex_t    bus_mutex;

void event_bus_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&bus_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    memset(subscribers, 0, sizeof(subscribers));
    global_gen  = 0;
    publishing  = false;
}

event_subscriber_t event_bus_subscribe(event_type_t type,
                                       event_handler_t handler,
                                       void *user_data)
{
    if (handler == NULL) return 0;

    pthread_mutex_lock(&bus_mutex);

    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (!subscribers[i].active) {
            uint32_t gen = global_gen++;
            if (gen >= MAX_GENERATION) gen = global_gen = 1;

            subscribers[i].generation = gen;
            subscribers[i].handle     = (gen << HANDLE_GEN_SHIFT) | ((uint32_t)(i + 1));
            subscribers[i].type       = type;
            subscribers[i].handler    = handler;
            subscribers[i].user_data  = user_data;
            subscribers[i].active     = true;
            pthread_mutex_unlock(&bus_mutex);
            return subscribers[i].handle;
        }
    }
    fprintf(stderr, "event_bus: out of subscriber slots\n");
    pthread_mutex_unlock(&bus_mutex);
    return 0;
}

void event_bus_unsubscribe(event_subscriber_t handle)
{
    if (handle == 0) return;

    pthread_mutex_lock(&bus_mutex);

    int      slot = (int)(handle & HANDLE_SLOT_MASK) - 1;
    uint32_t gen  = (uint32_t)(handle >> HANDLE_GEN_SHIFT);

    if (slot < 0 || slot >= EVENT_BUS_MAX_SUBSCRIBERS) {
        pthread_mutex_unlock(&bus_mutex);
        return;
    }
    if (!subscribers[slot].active) {
        pthread_mutex_unlock(&bus_mutex);
        return;
    }
    if (subscribers[slot].generation != gen) {  /* stale handle */
        pthread_mutex_unlock(&bus_mutex);
        return;
    }

    /* During publish, defer removal to avoid iterator corruption */
    if (publishing) {
        subscribers[slot].handler = NULL;  /* skip on next iteration */
        pthread_mutex_unlock(&bus_mutex);
        return;
    }

    subscribers[slot].active = false;
    pthread_mutex_unlock(&bus_mutex);
}

void event_bus_publish(const event_t *event)
{
    if (event == NULL) return;

    pthread_mutex_lock(&bus_mutex);

    bool was_publishing = publishing;
    publishing = true;

    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (subscribers[i].active &&
            subscribers[i].type == event->type &&
            subscribers[i].handler != NULL) {
            /* Call handler with lock held. Recursive mutex allows
             * handlers to call subscribe/unsubscribe/publish safely.
             * Deferred unsubscribe (handler=NULL) prevents iterator
             * corruption during in-publish unsubscription. */
            subscribers[i].handler(event, subscribers[i].user_data);
        }
    }

    publishing = was_publishing;

    /* Sweep — actually remove handlers that were unsubscribed during publish */
    if (!publishing) {
        for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
            if (subscribers[i].active && subscribers[i].handler == NULL) {
                subscribers[i].active = false;
            }
        }
    }

    pthread_mutex_unlock(&bus_mutex);
}

void event_bus_publish_int(event_type_t type, int value)
{
    event_t ev = { .type = type, .int_param = value };
    event_bus_publish(&ev);
}

void event_bus_publish_str(event_type_t type, const char *str)
{
    event_t ev = { .type = type };
    if (str) {
        strncpy(ev.str_param, str, sizeof(ev.str_param) - 1);
    }
    event_bus_publish(&ev);
}

void event_bus_deinit(void)
{
    pthread_mutex_lock(&bus_mutex);
    memset(subscribers, 0, sizeof(subscribers));
    global_gen  = 0;
    publishing  = false;
    pthread_mutex_unlock(&bus_mutex);
    pthread_mutex_destroy(&bus_mutex);
}
