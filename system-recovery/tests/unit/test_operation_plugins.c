/**
 * @file test_operation_plugins.c
 * @brief Unit tests for operation plugin registration and lookup.
 */

#include "test_framework.h"
#include "services/operations/op_interface.h"
#include <string.h>

/* ---- Dummy Plugin ----------------------------------------------------- */

static int dummy_validate(void) { return 0; }
static int dummy_init(void) { return 0; }
static operation_result_t dummy_execute(progress_callback_t p, void *c) {
    (void)p; (void)c;
    operation_result_t r = { .success = true, .error_code = 0 };
    strcpy(r.message, "OK");
    return r;
}
static void dummy_cleanup(void) { }

static operation_plugin_t dummy_plugin = {
    .name        = "dummy_op",
    .description = "A dummy operation for testing",
    .validate    = dummy_validate,
    .init        = dummy_init,
    .execute     = dummy_execute,
    .cleanup     = dummy_cleanup,
};

/* ---- Test Cases ------------------------------------------------------- */

static int test_register_and_find(void)
{
    operation_plugin_deregister_all();

    int ret = operation_plugin_register(&dummy_plugin);
    TEST_ASSERT_EQ(ret, 0, "register should succeed");

    const operation_plugin_t *found = operation_plugin_find("dummy_op");
    TEST_ASSERT_NOT_NULL(found, "should find registered plugin");
    TEST_ASSERT_STR_EQ(found->name, "dummy_op", "name should match");
    TEST_ASSERT_STR_EQ(found->description, "A dummy operation for testing",
                       "description should match");

    operation_plugin_deregister_all();
    return 0;
}

static int test_find_nonexistent(void)
{
    operation_plugin_deregister_all();

    const operation_plugin_t *found = operation_plugin_find("no_such_plugin");
    TEST_ASSERT_NULL(found, "nonexistent plugin should return NULL");

    operation_plugin_deregister_all();
    return 0;
}

static int test_execute_plugin(void)
{
    operation_plugin_deregister_all();

    operation_plugin_register(&dummy_plugin);
    const operation_plugin_t *p = operation_plugin_find("dummy_op");
    TEST_ASSERT_NOT_NULL(p, "should find plugin");

    TEST_ASSERT_EQ(p->validate(), 0, "validate should return 0");
    TEST_ASSERT_EQ(p->init(), 0, "init should return 0");

    operation_result_t r = p->execute(NULL, NULL);
    TEST_ASSERT_EQ(r.success, true, "execute should succeed");
    TEST_ASSERT_EQ(r.error_code, 0, "error_code should be 0");

    p->cleanup();

    operation_plugin_deregister_all();
    return 0;
}

static int test_deregister_all(void)
{
    operation_plugin_deregister_all();
    operation_plugin_register(&dummy_plugin);

    const operation_plugin_t *found = operation_plugin_find("dummy_op");
    TEST_ASSERT_NOT_NULL(found, "should find before deregister");

    operation_plugin_deregister_all();
    found = operation_plugin_find("dummy_op");
    TEST_ASSERT_NULL(found, "should not find after deregister all");

    return 0;
}

static int test_null_plugin_rejected(void)
{
    operation_plugin_deregister_all();
    int ret = operation_plugin_register(NULL);
    TEST_ASSERT_EQ(ret, -1, "NULL plugin should be rejected");
    operation_plugin_deregister_all();
    return 0;
}

static int registered = 0;
void register_plugin_tests(void)
{
    if (registered) return;
    REGISTER_TEST("operation_plugins", test_register_and_find);
    REGISTER_TEST("operation_plugins", test_find_nonexistent);
    REGISTER_TEST("operation_plugins", test_execute_plugin);
    REGISTER_TEST("operation_plugins", test_deregister_all);
    REGISTER_TEST("operation_plugins", test_null_plugin_rejected);
    registered = 1;
}
