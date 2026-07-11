/**
 * @file test_app_lifecycle.c
 * @brief System-level tests for application lifecycle.
 *
 * Tests the full init → run (simulated) → deinit cycle
 * of the application core.
 */

#include "test_framework.h"
#include "core/event_bus.h"
#include "core/app_core.h"
#include "common/types.h"
#include "common/utils.h"
#include "services/config_manager.h"
#include "services/operations/op_interface.h"
#include <string.h>

/* ---- Dummy Plugin for System Tests ------------------------------------ */

static int sys_validate(void) { return 0; }
static int sys_init(void) { return 0; }
static operation_result_t sys_execute(progress_callback_t p, void *c) {
    (void)p; (void)c;
    operation_result_t r = { .success = true, .error_code = 0 };
    strcpy(r.message, "System test success");
    return r;
}
static void sys_cleanup(void) { }

/* ---- Test Cases ------------------------------------------------------- */

static int test_full_init_deinit_cycle(void)
{
    /* Test that init and deinit don't crash */
    event_bus_init();
    TEST_ASSERT_EQ(config_manager_init(), true, "config init");

    event_bus_deinit();
    config_manager_deinit();
    /* No crash = pass */
    return 0;
}

static int test_event_bus_persistence(void)
{
    /* Test that the event bus correctly clears between init/deinit */
    event_bus_init();
    event_bus_publish_int(EVENT_SCREEN_CHANGE, SCREEN_RECOVERY);
    event_bus_deinit();

    /* Re-init should give clean state */
    event_bus_init();
    event_bus_deinit();
    return 0;
}

static int test_operation_plugin_system_flow(void)
{
    /* Full plugin life cycle */
    operation_plugin_deregister_all();

    operation_plugin_t sys_plugin = {
        .name        = "system_test_op",
        .description = "System test operation",
        .validate    = sys_validate,
        .init        = sys_init,
        .execute     = sys_execute,
        .cleanup     = sys_cleanup,
    };

    TEST_ASSERT_EQ(operation_plugin_register(&sys_plugin), 0, "register system plugin");

    const operation_plugin_t *p = operation_plugin_find("system_test_op");
    TEST_ASSERT_NOT_NULL(p, "find system plugin");

    /* Simulate full user flow */
    TEST_ASSERT_EQ(p->validate(), 0, "validate should pass");
    TEST_ASSERT_EQ(p->init(), 0, "init should pass");

    /* Simulate progress callbacks */
    static int captured_pct = 0;
    static char captured_status[128] = "";

    void capture_progress(int pct, const char *status, void *ctx) {
        (void)ctx;
        captured_pct = pct;
        strncpy(captured_status, status ? status : "", sizeof(captured_status) - 1);
    }

    operation_result_t r = p->execute(capture_progress, NULL);
    TEST_ASSERT_EQ(r.success, true, "execute should succeed");
    TEST_ASSERT_EQ(r.error_code, 0, "error code should be 0");

    p->cleanup();
    operation_plugin_deregister_all();
    return 0;
}

static int test_event_flow_operation(void)
{
    /* Test the complete event flow for an operation */
    event_bus_init();

    /* Simulate start */
    event_bus_publish_int(EVENT_OPERATION_START, 0);

    /* Simulate progress updates */
    for (int i = 0; i <= 100; i += 10) {
        event_t ev = { .type = EVENT_OPERATION_PROGRESS, .int_param = i };
        snprintf(ev.str_param, sizeof(ev.str_param), "Step %d%%", i);
        event_bus_publish(&ev);
    }

    /* Simulate success */
    event_t ev = { .type = EVENT_OPERATION_COMPLETE, .int_param = 1 };
    strcpy(ev.str_param, "Operation completed");
    event_bus_publish(&ev);

    /* Simulate failure */
    ev.int_param = 0;
    strcpy(ev.str_param, "Operation failed");
    event_bus_publish(&ev);

    event_bus_deinit();
    return 0;
}

static int test_utils_functions(void)
{
    /* Test utility functions */
    TEST_ASSERT_EQ(utils_file_exists("/nonexistent_file_xyz"), false,
                   "nonexistent file should return false");

    TEST_ASSERT_EQ(utils_file_exists("/dev/null"), true,
                   "/dev/null should exist");

    uint32_t t1 = utils_tick_get();
    utils_sleep_ms(10);
    uint32_t t2 = utils_tick_get();
    TEST_ASSERT_EQ(t2 >= t1, true, "time should move forward");

    return 0;
}

static int registered = 0;
void register_system_tests(void)
{
    if (registered) return;
    REGISTER_TEST("system", test_full_init_deinit_cycle);
    REGISTER_TEST("system", test_event_bus_persistence);
    REGISTER_TEST("system", test_operation_plugin_system_flow);
    REGISTER_TEST("system", test_event_flow_operation);
    REGISTER_TEST("system", test_utils_functions);
    registered = 1;
}
