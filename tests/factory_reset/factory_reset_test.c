#include <stdbool.h>
#include <stdint.h>

#include "factory_reset.h"
#include "unity.h"

void hal_gpio_mock_set_active(bool active);

static unsigned s_erase;
static unsigned s_restart;
static unsigned s_indicator;
static bool s_erase_result;

static bool erase(void *context) { (void)context; ++s_erase; return s_erase_result; }
static void indicator(bool active, void *context) { (void)context; if (active) ++s_indicator; }
static void restart(void *context) { (void)context; ++s_restart; }

void setUp(void)
{
    s_erase = 0u;
    s_restart = 0u;
    s_indicator = 0u;
    s_erase_result = true;
    hal_gpio_mock_set_active(false);
    factory_reset_deinit();
}

void tearDown(void) { factory_reset_deinit(); }

static void test_short_press_is_ignored(void)
{
    const factory_reset_config_t config = {
        .pin = 0u, .active_low = true, .hold_ms = 1000u,
        .erase = erase, .indicator = indicator, .restart = restart,
    };
    TEST_ASSERT_EQUAL_INT(FACTORY_RESET_OK, factory_reset_init(&config));
    hal_gpio_mock_set_active(true);
    factory_reset_poll(0u);
    factory_reset_poll(500u);
    hal_gpio_mock_set_active(false);
    factory_reset_poll(600u);
    TEST_ASSERT_EQUAL_UINT(0u, s_erase);
    TEST_ASSERT_EQUAL_UINT(0u, s_restart);
}

static void test_hold_erases_and_restarts_once(void)
{
    const factory_reset_config_t config = {
        .pin = 0u, .active_low = true, .hold_ms = 1000u,
        .erase = erase, .indicator = indicator, .restart = restart,
    };
    TEST_ASSERT_EQUAL_INT(FACTORY_RESET_OK, factory_reset_init(&config));
    hal_gpio_mock_set_active(true);
    factory_reset_poll(0u);
    factory_reset_poll(1051u);
    factory_reset_poll(2051u);
    TEST_ASSERT_EQUAL_UINT(1u, s_erase);
    TEST_ASSERT_EQUAL_UINT(1u, s_restart);
    TEST_ASSERT_TRUE(factory_reset_is_triggered());
}

static void test_failed_erase_requires_release_before_retry(void)
{
    const factory_reset_config_t config = {
        .pin = 0u, .active_low = true, .hold_ms = 1000u,
        .erase = erase, .indicator = indicator, .restart = restart,
    };
    TEST_ASSERT_EQUAL_INT(FACTORY_RESET_OK, factory_reset_init(&config));
    s_erase_result = false;
    hal_gpio_mock_set_active(true);
    factory_reset_poll(0u);
    factory_reset_poll(1051u);
    factory_reset_poll(1101u);
    factory_reset_poll(3000u);
    TEST_ASSERT_EQUAL_UINT(1u, s_erase);
    TEST_ASSERT_EQUAL_UINT(0u, s_restart);
    TEST_ASSERT_FALSE(factory_reset_is_triggered());

    s_erase_result = true;
    hal_gpio_mock_set_active(false);
    factory_reset_poll(3050u);
    hal_gpio_mock_set_active(true);
    factory_reset_poll(3100u);
    factory_reset_poll(4151u);
    TEST_ASSERT_EQUAL_UINT(2u, s_erase);
    TEST_ASSERT_EQUAL_UINT(1u, s_restart);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_press_is_ignored);
    RUN_TEST(test_hold_erases_and_restarts_once);
    RUN_TEST(test_failed_erase_requires_release_before_retry);
    return UNITY_END();
}
