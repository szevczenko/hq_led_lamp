/**
 * @file wifi_storage_overwrite_test.c
 * @brief Host test for the portal submit-overwrites-stale-credential path
 *        (TASK-136)
 *
 * This test compiles the REAL platform Wi-Fi manager (wifi_managment.c) and
 * its persistence module (wifi_config.c) and drives the exact submission
 * sequence the portal POST /api/v1/wifi/credentials handler executes
 * (platform/hq_platform/src/wifi_provisioning/wifi_http_provisioning.c:
 * wifi_mgmt_set_ap_name() + wifi_mgmt_set_password() + wifi_mgmt_connect())
 * against:
 *
 *   - a deterministic radio double (wifi_hal_mock_min.c) that rejects the
 *     stale credential and accepts the newly submitted one, and records the
 *     last station config the manager handed to the radio,
 *   - a real temp-directory-backed file system (wifi_storage_osal_support.c)
 *     that stands in for the product storage mount: lamp_fs mounts the
 *     LittleFS "storage" partition at /littlefs and the ESP OSAL maps the
 *     relative WIFI_CONFIG_FILE_PATH "wifi_ap.json" onto
 *     /littlefs/wifi_ap.json (osal_lfs_build_vfs_path); the host double
 *     applies the same logical-path mapping and the same CREATE|TRUNCATE
 *     open semantics against a real file,
 *   - the real platform POSIX OSAL back-ends for mutex / binary semaphore /
 *     task, so the manager's worker task, its semaphores and the
 *     connect/persist state machine behave exactly like on target.
 *
 * Scenario (the device state a user reaches after a password change or a
 * typo): the storage holds a STALE credential — the correct SSID with a
 * wrong password.  The station fails to connect with it and the platform
 * fallback controller opens the portal (AP+STA concurrent mode, the product
 * default).  The user submits the corrected credential through the portal.
 * The tests prove:
 *
 *   1. overwrite: the persistence list is updated and the storage file ends
 *      up holding exactly the newly submitted credential — the stale secret
 *      cannot survive (same-SSID submissions update the entry in place;
 *      different-SSID submissions become the last-used entry, which the
 *      boot-time loader selects),
 *   2. no-reboot switch: the SAME manager lifecycle connects with the new
 *      credential (the radio double records the station config actually
 *      applied after the submission), and the connect persists the entry in
 *      storage before the controller's SUCCEEDED event is even consumed by
 *      the supervisor (TASK-133 wiring),
 *   3. secrecy: neither the old nor the new password (nor the SSIDs) ever
 *      appears in any captured log line — the credentials-never-logged rule.
 *
 * This file is a host test only: it is compiled solely into the
 * wifi_storage_overwrite_tests binary and is never part of any production
 * build.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "wifi_config.h"
#include "wifi_hal_mock_min.h"
#include "wifi_managment.h"
#include "wifi_storage_osal_support.h"

/* --------------------------------------------------------------------- */
/* Test credentials                                                       */
/* --------------------------------------------------------------------- */

/* Same-SSID overwrite case: the classic stale-password scenario. */
#define OVERWRITE_SSID     "HomeNet"
#define OVERWRITE_OLD_PASS "stale-password-1"
#define OVERWRITE_NEW_PASS "correct-password-2"

/* Different-SSID rotation case: the submitted network replaces the stale
 * one as the last-used/selected credential. */
#define ROTATE_OLD_SSID    "OldNetwork"
#define ROTATE_OLD_PASS    "stale-password-3"
#define ROTATE_NEW_SSID    "NewNetwork"
#define ROTATE_NEW_PASS    "correct-password-4"

/* Fresh-submit case: no credential exists at boot; the portal submission
 * must persist the credential AND flip the manager's saved-credential flag
 * (wifi_mgmt_is_read_data), otherwise the machine parks at the NETWORK gate
 * after PROVISIONING_SUCCEEDED instead of proceeding to TLS (TASK-136). */
#define FRESH_SSID         "FreshNetwork"
#define FRESH_PASS         "fresh-password-5"

/* --------------------------------------------------------------------- */
/* Wait helpers                                                           */
/* --------------------------------------------------------------------- */

static bool wait_until(bool (*predicate)(void), uint32_t timeout_ms)
{
    uint32_t waited_ms = 0U;

    while (!predicate())
    {
        (void)usleep(20000U);
        waited_ms += 20U;
        if (waited_ms >= timeout_ms)
        {
            return false;
        }
    }
    return true;
}

static bool manager_trying_connect(void)
{
    return wifi_mgmt_trying_connect();
}

static bool manager_connected(void)
{
    return wifi_mgmt_is_connected();
}

/** @brief true once the storage file contains the given byte string. */
static bool storage_contains(const char *needle)
{
    char content[2048];
    long n = wifi_storage_test_read_file("wifi_ap.json", content,
                                         sizeof(content));

    return (n > 0L) && (strstr(content, needle) != NULL);
}

/** @brief The needle the storage wait is currently polling for. */
static const char *s_expected_storage_needle;

static bool storage_has_expected(void)
{
    return storage_contains(s_expected_storage_needle);
}

/** @brief Number of non-overlapping occurrences of @p needle in @p hay. */
static int count_substring(const char *hay, const char *needle)
{
    int count = 0;
    const char *cursor = hay;
    size_t needle_len = strlen(needle);

    if (needle_len == 0U)
    {
        return 0;
    }
    while ((cursor = strstr(cursor, needle)) != NULL)
    {
        ++count;
        cursor += needle_len;
    }
    return count;
}

/* --------------------------------------------------------------------- */
/* Fixtures                                                               */
/* --------------------------------------------------------------------- */

/** @brief Set by the CONNECT_FAILED subscription (stale round complete). */
static atomic_bool s_stale_round_failed;

static void on_connect_failed(wifi_mgmt_event_t event, void *user_data)
{
    (void)event;
    (void)user_data;
    atomic_store_explicit(&s_stale_round_failed, true, memory_order_release);
}

static bool stale_round_failed(void)
{
    return atomic_load_explicit(&s_stale_round_failed, memory_order_acquire);
}

/**
 * @brief Seed the storage volume with a stale saved credential.
 *
 * Writes the same JSON shape the platform persistence layer produces
 * ({"last_use":..., "credentials":[{"nb":..., "ssid":..., "password":...}]})
 * directly onto the logical path the Wi-Fi manager uses.
 */
static bool seed_stale_credential(const char *ssid, const char *password)
{
    const char *path = wifi_storage_test_mapped_path("wifi_ap.json");
    FILE *fh = fopen(path, "w");

    if (fh == NULL)
    {
        return false;
    }
    fprintf(fh,
            "{\"last_use\":0,\"credentials\":[{\"nb\":0,\"ssid\":\"%s\","
            "\"password\":\"%s\"}]}",
            ssid, password);
    fclose(fh);
    return true;
}

/**
 * @brief Boot the manager (AP+STA, as the product NETWORK gate does)
 *        against an already-mounted storage volume.
 *
 * The test harness mounts the storage volume in setUp() — mirroring the
 * product's FILESYSTEM gate — before this boots the manager, so the
 * init-time load (wifi_mgmt_init -> _load_saved_config) reads the mounted
 * wifi_ap.json exactly like the fixed product (TASK-136).
 */
static void boot_manager(void)
{
    /* The radio rejects every connect attempt by default (a wrong password
     * on a real AP); the submit path switches it to acceptance. */
    wifi_hal_mock_min_reset();
    wifi_hal_mock_min_set_connect_result(OSAL_ERROR);

    atomic_store_explicit(&s_stale_round_failed, false, memory_order_release);

    /* Product boot order (network_manager.c): set type -> init -> start. */
    wifi_mgmt_set_wifi_type(T_WIFI_TYPE_CLI_SER);
    wifi_mgmt_init();
    TEST_ASSERT_TRUE(wifi_mgmt_subscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                                         on_connect_failed, NULL));
    wifi_mgmt_start();
    TEST_ASSERT_TRUE(wifi_mgmt_wait_ready(3000U));
}

/**
 * @brief Boot the manager (AP+STA, as the product does) with a stale
 *        credential the radio rejects, and wait until the stale connect
 *        round has exhausted the connect budget — the fallback state in
 *        which the platform controller opens the provisioning portal.
 */
static void boot_with_stale_credential(const char *ssid,
                                       const char *password)
{
    TEST_ASSERT_TRUE(seed_stale_credential(ssid, password));
    boot_manager();

    /* The stale credential exhausts the connect budget and the manager
     * publishes CONNECT_FAILED — the trigger the platform fallback
     * controller reacts to by opening the portal. */
    TEST_ASSERT_TRUE_MESSAGE(wait_until(stale_round_failed, 5000U),
                             "stale credential never exhausted the connect "
                             "budget (no CONNECT_FAILED)");
}

/**
 * @brief Submit a credential exactly like the portal POST handler and drive
 *        the station connect to success in the same lifecycle.
 */
static void submit_like_portal_and_connect(const char *ssid,
                                           const char *password)
{
    wifi_hal_event_data_t ev;

    /* The portal unconditionally overwrites the station config and requests
     * an asynchronous connect (wifi_http_provisioning.c). */
    TEST_ASSERT_TRUE(wifi_mgmt_set_ap_name(ssid, strlen(ssid)));
    TEST_ASSERT_TRUE(wifi_mgmt_set_password(password, strlen(password)));
    TEST_ASSERT_TRUE(wifi_mgmt_connect());

    /* The radio now accepts the submitted credential. */
    wifi_hal_mock_min_set_connect_result(OSAL_SUCCESS);

    /* Wait until the manager is genuinely trying to connect with it, then
     * replay the radio's GOT_IP — the HAL event that marks the station as
     * connected and triggers the persist path (_save_current_sta_config). */
    TEST_ASSERT_TRUE_MESSAGE(wait_until(manager_trying_connect, 5000U),
                             "manager never started connecting after submit");

    memset(&ev, 0, sizeof(ev));
    (void)snprintf(ev.ip_info.ip, sizeof(ev.ip_info.ip), "192.168.42.7");
    (void)snprintf(ev.ip_info.netmask, sizeof(ev.ip_info.netmask),
                   "255.255.255.0");
    (void)snprintf(ev.ip_info.gw, sizeof(ev.ip_info.gw), "192.168.42.1");
    wifi_hal_mock_min_inject_event(WIFI_HAL_EVT_STA_GOT_IP, &ev);

    TEST_ASSERT_TRUE_MESSAGE(wait_until(manager_connected, 5000U),
                             "station never reached connected after GOT_IP");
}

void setUp(void)
{
    /* A fresh storage volume + captured log per test. */
    TEST_ASSERT_TRUE(wifi_storage_test_mount());
    wifi_storage_test_log_reset();
    s_expected_storage_needle = NULL;
}

void tearDown(void)
{
    /* End the manager lifecycle (stop, quiesce, delete worker) and drop the
     * storage volume.  tearDown must never fail the suite. */
    (void)wifi_mgmt_deinit();
    wifi_storage_test_unmount();
    wifi_storage_test_log_reset();
}

/* --------------------------------------------------------------------- */
/* TASK-136 overwrite tests                                               */
/* --------------------------------------------------------------------- */

/**
 * @brief Submit the corrected password for a network that already has a
 *        stale saved password: the storage ends up holding exactly the new
 *        credential, the station switches without a reboot, and no log line
 *        contains either secret.
 */
static void test_submit_overwrites_stale_credential(void)
{
    boot_with_stale_credential(OVERWRITE_SSID, OVERWRITE_OLD_PASS);

    /* Submit the corrected password for the SAME network. */
    submit_like_portal_and_connect(OVERWRITE_SSID, OVERWRITE_NEW_PASS);

    /* The connect persists the entry before the supervisor would poll the
     * controller's SUCCEEDED event (TASK-133): wait for the storage file to
     * carry the new secret. */
    s_expected_storage_needle = OVERWRITE_NEW_PASS;
    TEST_ASSERT_TRUE_MESSAGE(wait_until(storage_has_expected, 5000U),
                             "storage never persisted the new credential");

    /* 1. No-reboot switch: the radio double was handed exactly the newly
        submitted credential in the same manager lifecycle. */
    wifi_hal_sta_config_t last_cfg;
    TEST_ASSERT_TRUE(wifi_hal_mock_min_get_sta_config(&last_cfg));
    TEST_ASSERT_EQUAL_STRING(OVERWRITE_SSID, last_cfg.ssid);
    TEST_ASSERT_EQUAL_STRING(OVERWRITE_NEW_PASS, last_cfg.password);

    /* 2. Storage ends up holding EXACTLY the newly submitted credential:
        one entry, stale secret gone. */
    char content[2048];
    long n = wifi_storage_test_read_file("wifi_ap.json", content,
                                         sizeof(content));
    TEST_ASSERT_TRUE_MESSAGE(n > 0L, "wifi_ap.json missing after submit");
    TEST_ASSERT_NOT_NULL(strstr(content, OVERWRITE_NEW_PASS));
    TEST_ASSERT_NULL(strstr(content, OVERWRITE_OLD_PASS));
    TEST_ASSERT_EQUAL_INT(1, count_substring(content, "\"ssid\""));

    /* The reload path (what _load_saved_config does at boot) selects the
        single remaining entry — the newly submitted credential.  The in-place
        update bumps its sequence number (nb 0 -> 1) and promotes it to
        last_use, which is exactly what the boot loader reads. */
    wifi_config_list_t list;
    memset(&list, 0, sizeof(list));
    TEST_ASSERT_EQUAL_INT(OSAL_SUCCESS, wifi_config_load(&list));
    TEST_ASSERT_EQUAL_UINT(1u, list.count);
    TEST_ASSERT_EQUAL_STRING(OVERWRITE_SSID, list.entries[0].ssid);
    TEST_ASSERT_EQUAL_STRING(OVERWRITE_NEW_PASS, list.entries[0].password);
    wifi_config_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_TRUE(wifi_config_get_by_nb(&list, list.last_use, &entry));
    TEST_ASSERT_EQUAL_STRING(OVERWRITE_SSID, entry.ssid);
    TEST_ASSERT_EQUAL_STRING(OVERWRITE_NEW_PASS, entry.password);
    TEST_ASSERT_TRUE(wifi_mgmt_is_read_data());

    /* 3. No log line contains the old or new secret (or the SSID). */
    const char *log = wifi_storage_test_log_get();
    TEST_ASSERT_NULL(strstr(log, OVERWRITE_OLD_PASS));
    TEST_ASSERT_NULL(strstr(log, OVERWRITE_NEW_PASS));
    TEST_ASSERT_NULL(strstr(log, OVERWRITE_SSID));
}

/**
 * @brief Submit a DIFFERENT network while the stale credential exists: the
 *        submitted network becomes the last-used credential (what a reboot
 *        selects); the stale entry can only survive as an unselected
 *        rotation slot, and no log line contains either secret.
 */
static void test_submit_different_network_replaces_selected_credential(void)
{
    boot_with_stale_credential(ROTATE_OLD_SSID, ROTATE_OLD_PASS);

    /* Submit a different network through the portal path. */
    submit_like_portal_and_connect(ROTATE_NEW_SSID, ROTATE_NEW_PASS);

    /* The submitted network is what the connect persists. */
    s_expected_storage_needle = ROTATE_NEW_PASS;
    TEST_ASSERT_TRUE_MESSAGE(wait_until(storage_has_expected, 5000U),
                             "storage never persisted the new credential");

    /* The manager connected with the submitted network in the same
        lifecycle (no reboot). */
    wifi_hal_sta_config_t last_cfg;
    TEST_ASSERT_TRUE(wifi_hal_mock_min_get_sta_config(&last_cfg));
    TEST_ASSERT_EQUAL_STRING(ROTATE_NEW_SSID, last_cfg.ssid);
    TEST_ASSERT_EQUAL_STRING(ROTATE_NEW_PASS, last_cfg.password);

    /* The reload path (boot) selects the submitted network via last_use;
        the stale entry is no longer the selected credential. */
    wifi_config_list_t list;
    memset(&list, 0, sizeof(list));
    TEST_ASSERT_EQUAL_INT(OSAL_SUCCESS, wifi_config_load(&list));
    TEST_ASSERT_EQUAL_UINT(2u, list.count);
    wifi_config_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    TEST_ASSERT_TRUE(wifi_config_get_by_nb(&list, list.last_use, &entry));
    TEST_ASSERT_EQUAL_STRING(ROTATE_NEW_SSID, entry.ssid);
    TEST_ASSERT_EQUAL_STRING(ROTATE_NEW_PASS, entry.password);
    TEST_ASSERT_TRUE(wifi_mgmt_is_read_data());

    /* No log line contains the old or new secret (or either SSID). */
    const char *log = wifi_storage_test_log_get();
    TEST_ASSERT_NULL(strstr(log, ROTATE_OLD_PASS));
    TEST_ASSERT_NULL(strstr(log, ROTATE_NEW_PASS));
    TEST_ASSERT_NULL(strstr(log, ROTATE_OLD_SSID));
    TEST_ASSERT_NULL(strstr(log, ROTATE_NEW_SSID));
}

/**
 * @brief Submit a credential on a FRESH device (no credential exists at
 *        boot): the submission persists the credential on the mounted
 *        storage AND flips the manager's saved-credential flag
 *        (wifi_mgmt_is_read_data) so the post-SUCCEEDED NETWORK gate hands
 *        the station to TLS (TASK-136 flag fix in wifi_managment.c).  The
 *        station switches in the same lifecycle and no log line contains
 *        the secret or SSID.
 */
static void test_fresh_submit_reports_saved_credential(void)
{
    /* Fresh boot: the setUp-mounted storage volume holds NO credential. */
    boot_manager();

    /* The manager reports "fresh" — nothing was loaded from storage. */
    TEST_ASSERT_FALSE(wifi_mgmt_is_read_data());

    /* Portal submission on the same lifecycle. */
    submit_like_portal_and_connect(FRESH_SSID, FRESH_PASS);

    /* The connect persists the entry. */
    s_expected_storage_needle = FRESH_PASS;
    TEST_ASSERT_TRUE_MESSAGE(wait_until(storage_has_expected, 5000U),
                             "storage never persisted the fresh credential");

    /* The station switched with the submitted credential (no reboot). */
    wifi_hal_sta_config_t last_cfg;
    TEST_ASSERT_TRUE(wifi_hal_mock_min_get_sta_config(&last_cfg));
    TEST_ASSERT_EQUAL_STRING(FRESH_SSID, last_cfg.ssid);
    TEST_ASSERT_EQUAL_STRING(FRESH_PASS, last_cfg.password);

    /* TASK-136 flag fix: after a successful save the device reports
     * credentialed, so the NETWORK gate after PROVISIONING_SUCCEEDED passes
     * the station to TLS instead of parking forever. */
    TEST_ASSERT_TRUE(wifi_mgmt_is_read_data());

    /* Storage ends up holding exactly the submitted credential. */
    char content[2048];
    long n = wifi_storage_test_read_file("wifi_ap.json", content,
                                         sizeof(content));
    TEST_ASSERT_TRUE_MESSAGE(n > 0L, "wifi_ap.json missing after submit");
    TEST_ASSERT_EQUAL_INT(1, count_substring(content, "\"ssid\""));
    TEST_ASSERT_NOT_NULL(strstr(content, FRESH_PASS));

    /* Reload (what a boot reads) selects the submitted credential. */
    wifi_config_list_t list;
    memset(&list, 0, sizeof(list));
    TEST_ASSERT_EQUAL_INT(OSAL_SUCCESS, wifi_config_load(&list));
    TEST_ASSERT_EQUAL_UINT(1u, list.count);
    TEST_ASSERT_EQUAL_STRING(FRESH_SSID, list.entries[0].ssid);
    TEST_ASSERT_EQUAL_STRING(FRESH_PASS, list.entries[0].password);

    /* No log line contains the secret or the SSID. */
    const char *log = wifi_storage_test_log_get();
    TEST_ASSERT_NULL(strstr(log, FRESH_PASS));
    TEST_ASSERT_NULL(strstr(log, FRESH_SSID));
}

/* --------------------------------------------------------------------- */
/* Test registry                                                          */
/* --------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_submit_overwrites_stale_credential);
    RUN_TEST(test_submit_different_network_replaces_selected_credential);
    RUN_TEST(test_fresh_submit_reports_saved_credential);
    return UNITY_END();
}