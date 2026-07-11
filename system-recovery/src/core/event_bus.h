/**
 * @file event_bus.h
 * @brief Lightweight publish/subscribe event bus.
 *
 * All inter-module communication flows through the event bus.
 * Modules subscribe to event types they care about and publish
 * events when state changes.  This decouples the UI from
 * business logic and allows independent testing.
 */

#ifndef CORE_EVENT_BUS_H
#define CORE_EVENT_BUS_H

#include "common/types.h"

/** Maximum concurrent subscribers across all event types. */
#define EVENT_BUS_MAX_SUBSCRIBERS 32

/** Initialise the event bus. Must be called once before any other API. */
void event_bus_init(void);

/**
 * Subscribe to a specific event type.
 * @param type      Event to listen for.
 * @param handler   Callback invoked when the event is published.
 * @param user_data Opaque pointer passed back to the handler.
 * @return A subscriber handle (non-zero), or 0 on failure.
 */
event_subscriber_t event_bus_subscribe(event_type_t type,
                                       event_handler_t handler,
                                       void *user_data);

/** Unsubscribe a previously registered subscriber. */
void event_bus_unsubscribe(event_subscriber_t handle);

/**
 * Publish an event. All subscribers for event->type are called
 * synchronously in registration order.
 */
void event_bus_publish(const event_t *event);

/** Convenience: publish a simple event with just an int parameter. */
void event_bus_publish_int(event_type_t type, int value);

/** Convenience: publish a simple event with a string parameter. */
void event_bus_publish_str(event_type_t type, const char *str);

/** De-initialise the event bus, removing all subscribers. */
void event_bus_deinit(void);

#endif /* CORE_EVENT_BUS_H */
