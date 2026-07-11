/**
 * @file test_event_bus.c
 * @brief Unit tests for the event bus module.
 */

#include "test_framework.h"
#include "core/event_bus.h"
#include <string.h>

/* ---- Test data capture ------------------------------------------------ */

static int  g_last_int = -1;
static char g_last_str[128] = "";

static void capture_int(const event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev) g_last_int = ev->int_param;
}

static void capture_str(const event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev) strncpy(g_last_str, ev->str_param, sizeof(g_last_str) - 1);
}

/* ---- Test Cases ------------------------------------------------------- */

static int test_init_deinit(void)
{
    event_bus_init();
    event_bus_deinit();
    /* Should not crash */
    return 0;
}

static int test_subscribe_and_publish_int(void)
{
    event_bus_init();
    g_last_int = -1;

    event_subscriber_t sub = event_bus_subscribe(EVENT_OPERATION_PROGRESS,
                                                  capture_int, NULL);
    TEST_ASSERT_NOT_ZERO(sub, "subscribe should return non-zero handle");

    event_bus_publish_int(EVENT_OPERATION_PROGRESS, 42);
    TEST_ASSERT_EQ(g_last_int, 42, "handler should receive int value");

    event_bus_unsubscribe(sub);
    event_bus_publish_int(EVENT_OPERATION_PROGRESS, 99);
    TEST_ASSERT_EQ(g_last_int, 42, "unsubscribed handler should not be called");

    event_bus_deinit();
    return 0;
}

static int test_subscribe_and_publish_str(void)
{
    event_bus_init();
    g_last_str[0] = '\0';

    event_subscriber_t sub = event_bus_subscribe(EVENT_OPERATION_PROGRESS,
                                                  capture_str, NULL);
    TEST_ASSERT_NOT_ZERO(sub, "subscribe should succeed");

    event_bus_publish_str(EVENT_OPERATION_PROGRESS, "Hello World");
    TEST_ASSERT_STR_EQ(g_last_str, "Hello World", "handler should receive string");

    event_bus_unsubscribe(sub);
    event_bus_deinit();
    return 0;
}

static int test_multiple_subscribers(void)
{
    event_bus_init();
    g_last_int = -1;

    event_subscriber_t s1 = event_bus_subscribe(EVENT_SHUTDOWN, capture_int, NULL);
    event_subscriber_t s2 = event_bus_subscribe(EVENT_SHUTDOWN, capture_int, NULL);

    TEST_ASSERT_NOT_ZERO(s1, "first subscriber should register");
    TEST_ASSERT_NOT_ZERO(s2, "second subscriber should register");

    event_bus_publish_int(EVENT_SHUTDOWN, 7);
    /* Both handlers should fire; g_last_int captured from last one */
    TEST_ASSERT_EQ(g_last_int, 7, "last handler should receive value");

    event_bus_deinit();
    return 0;
}

static int test_no_subscriber_no_crash(void)
{
    event_bus_init();
    /* Publishing with no subscribers should not crash */
    event_bus_publish_int(EVENT_NONE, 5);
    event_bus_publish_str(EVENT_NONE, "test");
    event_bus_deinit();
    return 0;
}

/* ---- Registration ----------------------------------------------------- */

static int registered = 0;
void register_event_bus_tests(void)
{
    if (registered) return;
    REGISTER_TEST("event_bus", test_init_deinit);
    REGISTER_TEST("event_bus", test_subscribe_and_publish_int);
    REGISTER_TEST("event_bus", test_subscribe_and_publish_str);
    REGISTER_TEST("event_bus", test_multiple_subscribers);
    REGISTER_TEST("event_bus", test_no_subscriber_no_crash);
    registered = 1;
}
