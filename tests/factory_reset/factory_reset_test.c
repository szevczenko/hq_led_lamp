#include <stdbool.h>
#include <stdint.h>

#include "factory_reset.h"
#include "hq_factory_reset.h"
#include "unity.h"

void hal_gpio_mock_set_active(bool active);

static unsigned s_erase;
static unsigned s_restart;
static unsigned s_indicator;
static bool s_erase_result;
static bool s_enabled;

static bool erase(void *context) { (void)context; ++s_erase; return s_erase_result; }
static void indicator(bool active, void *context) { (void)context; if (active) ++s_indicator; }
static void restart(void *context) { (void)context; ++s_restart; }
static bool enabled(void *context) { (void)context; return s_enabled; }

void setUp(void)
{
    s_erase = 0u;
    s_restart = 0u;
    s_indicator = 0u;
    s_erase_result = true;
    s_enabled = true;
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
    factory_reset_poll(3100u);
    hal_gpio_mock_set_active(true);
    factory_reset_poll(3150u);
    factory_reset_poll(4200u);
    TEST_ASSERT_EQUAL_UINT(2u, s_erase);
    TEST_ASSERT_EQUAL_UINT(1u, s_restart);
}

static void test_platform_service_honors_debounce_and_enable_gate(void)
{
    hq_factory_reset_t service = {0};
    const hq_factory_reset_config_t config = {
        .pin = 0u,
        .polarity = HAL_POLARITY_ACTIVE_LOW,
        .hold_ms = 200u,
        .debounce_ms = 100u,
        .erase = erase,
        .indicator = indicator,
        .restart = restart,
        .is_enabled = enabled,
    };

    s_enabled = false;
    hal_gpio_mock_set_active(true);
    TEST_ASSERT_EQUAL_INT(HQ_FACTORY_RESET_OK, hq_factory_reset_init(&service, &config));
    hq_factory_reset_poll(&service, 0u);
    hq_factory_reset_poll(&service, 400u);
    TEST_ASSERT_EQUAL_UINT(0u, s_indicator);
    TEST_ASSERT_EQUAL_UINT(0u, s_erase);

    s_enabled = true;
    hq_factory_reset_poll(&service, 500u);
    hq_factory_reset_poll(&service, 599u);
    TEST_ASSERT_EQUAL_UINT(0u, s_indicator);
    hq_factory_reset_poll(&service, 600u);
    TEST_ASSERT_EQUAL_UINT(1u, s_indicator);
    hq_factory_reset_poll(&service, 799u);
    TEST_ASSERT_EQUAL_UINT(0u, s_erase);
    hq_factory_reset_poll(&service, 800u);
    TEST_ASSERT_EQUAL_UINT(1u, s_erase);
    TEST_ASSERT_EQUAL_UINT(1u, s_restart);
    hq_factory_reset_deinit(&service);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_press_is_ignored);
    RUN_TEST(test_hold_erases_and_restarts_once);
    RUN_TEST(test_failed_erase_requires_release_before_retry);
    RUN_TEST(test_platform_service_honors_debounce_and_enable_gate);
    return UNITY_END();
}
