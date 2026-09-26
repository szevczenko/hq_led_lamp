/**
 * @file ota_manager_test.c
 * @brief Host tests for the ThingsBoard OTA supervisor (ota_manager).
 */

#include <string.h>

#include "unity.h"

#include "ota_manager.h"
#include "tb_firmware_update_mock.h"

#define CHECK_PERIOD_MS  60000u
#define REBOOT_DELAY_MS  2000u
#define BLINK_PERIOD_MS  1000u

static tb_client_t *const CLIENT_A = (tb_client_t *)0x1000;
static tb_client_t *const CLIENT_B = (tb_client_t *)0x2000;

static uint32_t s_now_ms;
static int s_events[16];
static int s_event_count;
static int s_indicator_calls;
static bool s_indicator_on;
static int s_restart_calls;

static uint32_t test_now(void)
{
    return s_now_ms;
}

static void test_on_event(ota_manager_event_t event, void *ctx)
{
    (void)ctx;
    if (s_event_count < (int)(sizeof(s_events) / sizeof(s_events[0])))
    {
        s_events[s_event_count] = (int)event;
    }
    s_event_count++;
}

static void test_on_indicator(bool on, void *ctx)
{
    (void)ctx;
    s_indicator_calls++;
    s_indicator_on = on;
}

static void test_restart(void)
{
    s_restart_calls++;
}

static ota_manager_config_t make_cfg(void)
{
    const ota_manager_config_t cfg = {
        .title = "kitchen_led_controller",
        .version = "1.0.0",
        .chunk_size = 2048u,
        .chunk_timeout_ms = 5000u,
        .chunk_retries = 4u,
        .check_period_ms = CHECK_PERIOD_MS,
        .reboot_delay_ms = REBOOT_DELAY_MS,
        .blink_period_ms = BLINK_PERIOD_MS,
        .now_ms = test_now,
        .on_event = test_on_event,
        .on_indicator = test_on_indicator,
        .restart = test_restart,
    };
    return cfg;
}

static void init_and_connect(void)
{
    const ota_manager_config_t cfg = make_cfg();
    TEST_ASSERT_EQUAL(OTA_MANAGER_OK, ota_manager_init(&cfg));
    ota_manager_on_connected(CLIENT_A);
}

static void start_download(void)
{
    fw_mock_set_state(TB_FIRMWARE_UPDATE_STATE_DOWNLOADING, 0u, 1000u, NULL);
    ota_manager_poll();
}

void setUp(void)
{
    fw_mock_reset();
    s_now_ms = 1000u;
    memset(s_events, 0, sizeof(s_events));
    s_event_count = 0;
    s_indicator_calls = 0;
    s_indicator_on = false;
    s_restart_calls = 0;
}

void tearDown(void)
{
    ota_manager_deinit();
}

static void test_init_rejects_invalid_config(void)
{
    ota_manager_config_t cfg = make_cfg();

    TEST_ASSERT_EQUAL(OTA_MANAGER_ERR_INVALID_ARGUMENT, ota_manager_init(NULL));
    cfg.restart = NULL;
    TEST_ASSERT_EQUAL(OTA_MANAGER_ERR_INVALID_ARGUMENT, ota_manager_init(&cfg));
    cfg = make_cfg();
    cfg.version = NULL;
    TEST_ASSERT_EQUAL(OTA_MANAGER_ERR_INVALID_ARGUMENT, ota_manager_init(&cfg));
    cfg = make_cfg();
    TEST_ASSERT_EQUAL(OTA_MANAGER_OK, ota_manager_init(&cfg));
    TEST_ASSERT_EQUAL(OTA_MANAGER_ERR_ALREADY_INITIALIZED,
                      ota_manager_init(&cfg));
}

static void test_connect_initializes_updater_with_config(void)
{
    init_and_connect();

    TEST_ASSERT_EQUAL(1, g_fw_mock.init_calls);
    TEST_ASSERT_EQUAL_PTR(CLIENT_A, g_fw_mock.last_client);
    TEST_ASSERT_EQUAL_STRING("kitchen_led_controller", g_fw_mock.last_title);
    TEST_ASSERT_EQUAL_STRING("1.0.0", g_fw_mock.last_version);
    TEST_ASSERT_EQUAL_UINT32(2048u, g_fw_mock.last_cfg.chunk_size);
    TEST_ASSERT_EQUAL_UINT32(5000u, g_fw_mock.last_cfg.chunk_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(4u, g_fw_mock.last_cfg.max_chunk_retries);
    TEST_ASSERT_NOT_NULL(g_fw_mock.last_cfg.on_reboot_required);
    TEST_ASSERT_FALSE(ota_manager_is_active());

    /* A reconnect re-initializes after tearing the previous updater down. */
    ota_manager_on_connected(CLIENT_B);
    TEST_ASSERT_EQUAL(1, g_fw_mock.deinit_calls);
    TEST_ASSERT_EQUAL(2, g_fw_mock.init_calls);
    TEST_ASSERT_EQUAL_PTR(CLIENT_B, g_fw_mock.last_client);
}

static void test_requested_check_issued_once(void)
{
    init_and_connect();
    ota_manager_poll();
    TEST_ASSERT_EQUAL(0, g_fw_mock.request_check_calls);

    ota_manager_request_check();
    ota_manager_request_check();
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, g_fw_mock.request_check_calls);
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, g_fw_mock.request_check_calls);
    TEST_ASSERT_EQUAL(3, g_fw_mock.poll_calls);
}

static void test_request_before_connect_is_kept(void)
{
    const ota_manager_config_t cfg = make_cfg();
    TEST_ASSERT_EQUAL(OTA_MANAGER_OK, ota_manager_init(&cfg));

    ota_manager_request_check();
    ota_manager_poll();
    TEST_ASSERT_EQUAL(0, g_fw_mock.request_check_calls);

    ota_manager_on_connected(CLIENT_A);
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, g_fw_mock.request_check_calls);
}

static void test_periodic_check(void)
{
    init_and_connect();

    s_now_ms += CHECK_PERIOD_MS - 1u;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(0, g_fw_mock.request_check_calls);
    s_now_ms += 1u;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, g_fw_mock.request_check_calls);
    s_now_ms += CHECK_PERIOD_MS / 2u;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, g_fw_mock.request_check_calls);
    s_now_ms += CHECK_PERIOD_MS / 2u;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(2, g_fw_mock.request_check_calls);
}

static void test_periodic_check_disabled(void)
{
    ota_manager_config_t cfg = make_cfg();
    cfg.check_period_ms = 0u;
    TEST_ASSERT_EQUAL(OTA_MANAGER_OK, ota_manager_init(&cfg));
    ota_manager_on_connected(CLIENT_A);

    s_now_ms += 10u * CHECK_PERIOD_MS;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(0, g_fw_mock.request_check_calls);
}

static void test_download_start_blinks_and_suppresses_checks(void)
{
    init_and_connect();
    start_download();

    TEST_ASSERT_TRUE(ota_manager_is_active());
    TEST_ASSERT_EQUAL(1, s_event_count);
    TEST_ASSERT_EQUAL(OTA_MANAGER_EVENT_STARTED, s_events[0]);
    TEST_ASSERT_TRUE(s_indicator_on);
    TEST_ASSERT_EQUAL(1, s_indicator_calls);

    s_now_ms += BLINK_PERIOD_MS - 1u;
    ota_manager_poll();
    TEST_ASSERT_TRUE(s_indicator_on);
    s_now_ms += 1u;
    ota_manager_poll();
    TEST_ASSERT_FALSE(s_indicator_on);
    s_now_ms += BLINK_PERIOD_MS;
    ota_manager_poll();
    TEST_ASSERT_TRUE(s_indicator_on);

    ota_manager_request_check();
    s_now_ms += CHECK_PERIOD_MS;
    fw_mock_set_state(TB_FIRMWARE_UPDATE_STATE_DOWNLOADING, 500u, 1000u, NULL);
    ota_manager_poll();
    TEST_ASSERT_EQUAL(0, g_fw_mock.request_check_calls);
    TEST_ASSERT_EQUAL(1, s_event_count);
}

static void test_download_failure_returns_to_normal(void)
{
    init_and_connect();
    start_download();

    fw_mock_set_state(TB_FIRMWARE_UPDATE_STATE_FAILED, 400u, 1000u,
                      "firmware checksum mismatch");
    ota_manager_poll();

    TEST_ASSERT_FALSE(ota_manager_is_active());
    TEST_ASSERT_EQUAL(2, s_event_count);
    TEST_ASSERT_EQUAL(OTA_MANAGER_EVENT_FAILED, s_events[1]);
    TEST_ASSERT_FALSE(s_indicator_on);
    TEST_ASSERT_EQUAL(0, s_restart_calls);

    /* A pending check request resumes once the update ended. */
    ota_manager_request_check();
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, g_fw_mock.request_check_calls);
}

static void test_reboot_required_restarts_after_delay(void)
{
    init_and_connect();
    start_download();
    fw_mock_set_state(TB_FIRMWARE_UPDATE_STATE_UPDATING, 1000u, 1000u, NULL);
    fw_mock_fire_reboot_required();

    ota_manager_poll();
    TEST_ASSERT_TRUE(ota_manager_is_active());
    TEST_ASSERT_EQUAL(0, s_restart_calls);
    const int polls = g_fw_mock.poll_calls;

    s_now_ms += REBOOT_DELAY_MS - 1u;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(0, s_restart_calls);
    TEST_ASSERT_EQUAL(polls, g_fw_mock.poll_calls);

    s_now_ms += 1u;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, s_restart_calls);
    TEST_ASSERT_EQUAL(2, s_event_count);
    TEST_ASSERT_EQUAL(OTA_MANAGER_EVENT_RESTARTING, s_events[1]);
    TEST_ASSERT_FALSE(s_indicator_on);
}

static void test_reboot_required_without_seen_download_starts_first(void)
{
    init_and_connect();
    fw_mock_set_state(TB_FIRMWARE_UPDATE_STATE_UPDATING, 1000u, 1000u, NULL);
    fw_mock_fire_reboot_required();
    ota_manager_poll();

    TEST_ASSERT_EQUAL(1, s_event_count);
    TEST_ASSERT_EQUAL(OTA_MANAGER_EVENT_STARTED, s_events[0]);
    s_now_ms += REBOOT_DELAY_MS;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, s_restart_calls);
    TEST_ASSERT_EQUAL(OTA_MANAGER_EVENT_RESTARTING, s_events[1]);
}

static void test_disconnect_during_download_aborts(void)
{
    init_and_connect();
    start_download();

    ota_manager_on_disconnected();
    ota_manager_poll();

    TEST_ASSERT_EQUAL(1, g_fw_mock.deinit_calls);
    TEST_ASSERT_FALSE(ota_manager_is_active());
    TEST_ASSERT_EQUAL(OTA_MANAGER_EVENT_FAILED, s_events[1]);

    /* Nothing is polled or requested while disconnected. */
    const int polls = g_fw_mock.poll_calls;
    ota_manager_request_check();
    ota_manager_poll();
    TEST_ASSERT_EQUAL(polls, g_fw_mock.poll_calls);
    TEST_ASSERT_EQUAL(0, g_fw_mock.request_check_calls);

    ota_manager_on_connected(CLIENT_A);
    fw_mock_set_state(TB_FIRMWARE_UPDATE_STATE_FAILED, 0u, 0u,
                      "firmware update stopped during deinitialization");
    ota_manager_poll();
    TEST_ASSERT_EQUAL(2, g_fw_mock.init_calls);
    TEST_ASSERT_EQUAL(1, g_fw_mock.request_check_calls);
    TEST_ASSERT_EQUAL(2, s_event_count);
}

static void test_reconnect_during_download_fails_then_reinits(void)
{
    init_and_connect();
    start_download();

    ota_manager_on_connected(CLIENT_A);
    TEST_ASSERT_FALSE(ota_manager_is_active());
    TEST_ASSERT_EQUAL(OTA_MANAGER_EVENT_FAILED, s_events[1]);
    TEST_ASSERT_EQUAL(2, g_fw_mock.init_calls);
}

static void test_init_failure_retried(void)
{
    const ota_manager_config_t cfg = make_cfg();
    TEST_ASSERT_EQUAL(OTA_MANAGER_OK, ota_manager_init(&cfg));
    g_fw_mock.init_result = -1;
    ota_manager_on_connected(CLIENT_A);
    TEST_ASSERT_EQUAL(1, g_fw_mock.init_calls);

    g_fw_mock.init_result = 0;
    s_now_ms += OTA_MANAGER_INIT_RETRY_MS - 1u;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, g_fw_mock.init_calls);
    s_now_ms += 1u;
    ota_manager_poll();
    TEST_ASSERT_EQUAL(2, g_fw_mock.init_calls);
    ota_manager_request_check();
    ota_manager_poll();
    TEST_ASSERT_EQUAL(1, g_fw_mock.request_check_calls);
}

static void test_defaults_applied(void)
{
    ota_manager_config_t cfg = make_cfg();
    cfg.chunk_size = 0u;
    TEST_ASSERT_EQUAL(OTA_MANAGER_OK, ota_manager_init(&cfg));
    ota_manager_on_connected(CLIENT_A);
    TEST_ASSERT_EQUAL_UINT32(OTA_MANAGER_CHUNK_SIZE_DEFAULT,
                             g_fw_mock.last_cfg.chunk_size);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_invalid_config);
    RUN_TEST(test_connect_initializes_updater_with_config);
    RUN_TEST(test_requested_check_issued_once);
    RUN_TEST(test_request_before_connect_is_kept);
    RUN_TEST(test_periodic_check);
    RUN_TEST(test_periodic_check_disabled);
    RUN_TEST(test_download_start_blinks_and_suppresses_checks);
    RUN_TEST(test_download_failure_returns_to_normal);
    RUN_TEST(test_reboot_required_restarts_after_delay);
    RUN_TEST(test_reboot_required_without_seen_download_starts_first);
    RUN_TEST(test_disconnect_during_download_aborts);
    RUN_TEST(test_reconnect_during_download_fails_then_reinits);
    RUN_TEST(test_init_failure_retried);
    RUN_TEST(test_defaults_applied);
    return UNITY_END();
}
