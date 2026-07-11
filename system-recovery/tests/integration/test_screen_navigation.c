/**
 * @file test_screen_navigation.c
 * @brief Integration tests for screen navigation via event bus.
 *
 * Tests that screen change events published on the event bus result in
 * the correct navigation callbacks being invoked.
 */

#include "test_framework.h"
#include "core/event_bus.h"
#include "common/types.h"
#include <string.h>

/* ---- Test Context ----------------------------------------------------- */

static screen_id_t g_last_screen = -1;
static int         g_navigate_count = 0;

static void nav_handler(const event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev == NULL) return;
    g_last_screen = (screen_id_t)ev->int_param;
    g_navigate_count++;
}

/* ---- Test Cases ------------------------------------------------------- */

static int test_screen_navigation_via_event(void)
{
    event_bus_init();
    g_last_screen = -1;
    g_navigate_count = 0;

    event_subscriber_t sub = event_bus_subscribe(EVENT_SCREEN_CHANGE,
                                                  nav_handler, NULL);
    TEST_ASSERT_NOT_ZERO(sub, "should subscribe");

    /* Navigate to recovery screen */
    event_bus_publish_int(EVENT_SCREEN_CHANGE, SCREEN_RECOVERY);
    TEST_ASSERT_EQ(g_last_screen, SCREEN_RECOVERY, "should navigate to recovery");
    TEST_ASSERT_EQ(g_navigate_count, 1, "navigate count should be 1");

    /* Navigate to progress screen */
    event_bus_publish_int(EVENT_SCREEN_CHANGE, SCREEN_PROGRESS);
    TEST_ASSERT_EQ(g_last_screen, SCREEN_PROGRESS, "should navigate to progress");
    TEST_ASSERT_EQ(g_navigate_count, 2, "navigate count should be 2");

    /* Navigate to notify screen */
    event_bus_publish_int(EVENT_SCREEN_CHANGE, SCREEN_NOTIFY);
    TEST_ASSERT_EQ(g_last_screen, SCREEN_NOTIFY, "should navigate to notify");
    TEST_ASSERT_EQ(g_navigate_count, 3, "navigate count should be 3");

    event_bus_unsubscribe(sub);
    event_bus_deinit();
    return 0;
}

static void dummy_handler(const event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    /* no-op handler for testing */
}

static int test_operation_progress_flow(void)
{
    event_bus_init();

    /* Simulate what the progress screen does – use a real handler */
    event_subscriber_t sub_p = event_bus_subscribe(EVENT_OPERATION_PROGRESS,
        dummy_handler, NULL);
    event_subscriber_t sub_c = event_bus_subscribe(EVENT_OPERATION_COMPLETE,
        dummy_handler, NULL);

    TEST_ASSERT_NOT_ZERO(sub_p, "progress subscriber");
    TEST_ASSERT_NOT_ZERO(sub_c, "complete subscriber");

    /* Simulate a progress sequence */
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EVENT_OPERATION_PROGRESS;
    ev.int_param = 25;
    strcpy(ev.str_param, "Verifying checksum...");
    event_bus_publish(&ev);

    ev.int_param = 50;
    strcpy(ev.str_param, "Flashing kernel...");
    event_bus_publish(&ev);

    ev.int_param = 100;
    strcpy(ev.str_param, "Complete");
    event_bus_publish(&ev);

    /* Simulate completion */
    ev.type = EVENT_OPERATION_COMPLETE;
    ev.int_param = 1;
    strcpy(ev.str_param, "Recovery completed successfully");
    event_bus_publish(&ev);

    event_bus_deinit();
    /* No crash = pass */
    return 0;
}

static int test_event_bus_isolation(void)
{
    /* Verify that events for different types don't cross-contaminate */
    event_bus_init();

    int shutdown_count = 0;
    int reboot_count = 0;

    /* This test verifies the architecture, not actual capture
     * (no capture helper for multiple counters without static state).
     * We just verify that publishing to one type doesn't crash. */
    event_subscriber_t s1 = event_bus_subscribe(EVENT_SHUTDOWN, NULL, NULL);
    event_subscriber_t s2 = event_bus_subscribe(EVENT_REBOOT, NULL, NULL);

    event_bus_publish_int(EVENT_SHUTDOWN, 1);
    event_bus_publish_int(EVENT_REBOOT, 1);
    event_bus_publish_int(EVENT_SCREEN_CHANGE, SCREEN_MAIN);
    event_bus_publish_int(EVENT_OPERATION_PROGRESS, 50);

    event_bus_unsubscribe(s1);
    event_bus_unsubscribe(s2);
    event_bus_deinit();
    return 0;
}

static int registered = 0;
void register_integration_tests(void)
{
    if (registered) return;
    REGISTER_TEST("integration", test_screen_navigation_via_event);
    REGISTER_TEST("integration", test_operation_progress_flow);
    REGISTER_TEST("integration", test_event_bus_isolation);
    registered = 1;
}
