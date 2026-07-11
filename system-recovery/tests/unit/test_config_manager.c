/**
 * @file test_config_manager.c
 * @brief Unit tests for the configuration manager.
 */

#include "test_framework.h"
#include "services/config_manager.h"
#include <string.h>

static int test_init_deinit(void)
{
    TEST_ASSERT_EQ(config_manager_init(), true, "init should succeed");
    config_manager_deinit();
    return 0;
}

static int test_default_values(void)
{
    config_manager_init();

    const char *val = config_get_string("nonexistent", "key", "default_val");
    TEST_ASSERT_STR_EQ(val, "default_val", "missing key should return default");

    int ival = config_get_int("nonexistent", "key2", 42);
    TEST_ASSERT_EQ(ival, 42, "missing int key should return default");

    bool bval = config_get_bool("nonexistent", "key3", true);
    TEST_ASSERT_EQ(bval, true, "missing bool key should return default");

    config_manager_deinit();
    return 0;
}

static int test_well_known_keys(void)
{
    config_manager_init();

    /* Default values for well-known keys (no config file loaded in tests) */
    int sens = config_get_int(CFG_TOUCHPAD_SENSITIVITY, 100);
    TEST_ASSERT_EQ(sens, 100, "default touchpad sensitivity should be 100");

    int dbl = config_get_int(CFG_DOUBLE_CLICK_MS, 450);
    TEST_ASSERT_EQ(dbl, 450, "default double click ms should be 450");

    bool debug = config_get_bool(CFG_INPUT_DEBUG, false);
    TEST_ASSERT_EQ(debug, false, "default input debug should be false");

    const char *part = config_get_string(CFG_RECOVERY_PARTITION, "/dev/mmcblk0p2");
    TEST_ASSERT_STR_EQ(part, "/dev/mmcblk0p2", "default recovery partition");

    config_manager_deinit();
    return 0;
}

static int test_bool_parsing(void)
{
    config_manager_init();

    /* Environment overrides via setenv (simulated by config key injection
     * – in real usage the config manager reads env vars in init).
     * Here we just test the default bool logic is sensible. */
    bool v = config_get_bool("test", "bool_true", true);
    TEST_ASSERT_EQ(v, true, "unknown key with default true");

    v = config_get_bool("test", "bool_false", false);
    TEST_ASSERT_EQ(v, false, "unknown key with default false");

    config_manager_deinit();
    return 0;
}

static int registered = 0;
void register_config_tests(void)
{
    if (registered) return;
    REGISTER_TEST("config_manager", test_init_deinit);
    REGISTER_TEST("config_manager", test_default_values);
    REGISTER_TEST("config_manager", test_well_known_keys);
    REGISTER_TEST("config_manager", test_bool_parsing);
    registered = 1;
}
