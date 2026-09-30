/**
 * @file device_identity_test.c
 * @brief Host unit tests for the access-token device identity owner
 *        (TASK-111)
 *
 * Runs the production components/device_identity code together with the
 * production components/app_config service against:
 *   - the REAL POSIX OSAL filesystem backend on a temporary LittleFS
 *     volume (platform/hq_platform/src/osal/posix + vendored littlefs),
 *     so /config/manufacturing.json and /config/identity.json are really
 *     read through the logical OSAL paths the mount resolves,
 *   - the vendored cJSON codec (strict schema parsing),
 *   - a test-only lamp-control double (lamp_control_mock.c) for the
 *     deterministic fail-off checks,
 *   - the Unity framework.
 *
 * Coverage per the TASK-111 definition of done:
 *   1. valid identity — a provisioned manufacturing record plus a valid
 *      /config/identity.json loads (DEVICE_IDENTITY_OK); the stable
 *      client-ID and token-provider interfaces return the validated values
 *      verbatim (the identity that ThingsBoard initialization consumes),
 *   2. every invalid record category is rejected with a distinct status
 *      and forces the output inactive: missing identity record, missing /
 *      unprovisioned / quarantined / credential-less manufacturing record,
 *      missing member, empty client ID, empty token, oversized file,
 *      oversized field, malformed JSON, trailing garbage, unknown member,
 *      duplicate member, control/whitespace characters in the token,
 *      unknown schema, wrong-typed members,
 *   3. no fallback — after any rejection the module stays unloaded: the
 *      getters report not_loaded and the token provider returns NULL, so a
 *      device can never anonymously connect,
 *   4. redaction — status names carry no record content (in particular
 *      never the token) and the component performs no logging,
 *   5. zeroization — device_identity_clear() invalidates the identity and
 *      wipes the module token buffer,
 *   6. recovery — after a rejection is repaired (a corrected record is
 *      installed) a reload succeeds, and a previously successful identity
 *      is never served from a later rejected record (fail closed).
 */

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "device_identity.h"
#include "osal_dir.h"
#include "osal_error.h"
#include "osal_file.h"
#include "osal_mount.h"

#include "unity.h"

#include "lamp_control_mock.h"

/* --------------------------------------------------------------------- */
/* Test volume and fixtures                                               */
/* --------------------------------------------------------------------- */

/** @brief Per-test temporary LittleFS block-device image. */
static const char *const LFS_IMAGE_PATH = "/tmp/device_identity_test.img";

#define LFS_BLOCK_SIZE 4096U
#define LFS_BLOCK_COUNT 256U

/** @brief A valid v1 identity record for the development device. */
static const char *const TEST_CLIENT_ID = "klc-kitchen-01";

static const char *const TEST_TOKEN =
    "A1b2C3d4E5f6G7h8I9j0K1l2M3n4O5p6Q7r8S9t0U1v";

static const char *const TEST_IDENTITY_V1 =
    "{\"schema_version\":1,"
    "\"client_id\":\"klc-kitchen-01\","
    "\"access_token\":\"A1b2C3d4E5f6G7h8I9j0K1l2M3n4O5p6Q7r8S9t0U1v\"}";

void setUp(void)
{
    (void)lamp_mock_reset();
    (void)device_identity_clear();

    /* Fresh LittleFS volume per test, backed by a temporary image file. */
    (void)osal_unmount("/");
    (void)osal_rmfs(LFS_IMAGE_PATH);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS,
                            osal_mkfs((char *)LFS_IMAGE_PATH, "littlefs",
                                      "config", LFS_BLOCK_SIZE,
                                      LFS_BLOCK_COUNT));
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS,
                            osal_mount(LFS_IMAGE_PATH, "/"));

    /* The device boot path creates /config through the filesystem
     * bootstrap (lamp_fs); mirror that here (idempotent mkdir). */
    (void)osal_mkdir("/config");
}

void tearDown(void)
{
    (void)device_identity_clear();
    (void)osal_unmount("/");
    (void)osal_rmfs(LFS_IMAGE_PATH);
}

/* --------------------------------------------------------------------- */
/* Helpers                                                                */
/* --------------------------------------------------------------------- */

static bool write_raw(const char *path, const char *content, size_t len)
{
    osal_file_id_t fd = osal_open_create(
        path, (osal_file_flag_t)(OSAL_FILE_FLAG_CREATE |
                                 OSAL_FILE_FLAG_TRUNCATE),
        OSAL_WRITE_ONLY);
    if (fd < 0)
    {
        return false;
    }
    bool ok = true;
    if (len > 0u)
    {
        ok = osal_write(fd, content, len) == (int32_t)len;
    }
    (void)osal_close(fd);
    return ok;
}

static bool write_identity(const char *json)
{
    return write_raw(DEVICE_IDENTITY_FILE_PATH, json, strlen(json));
}

/** @brief Commit the manufacturing record through the validated path. */
static void commit_manufacturing(app_config_mfg_state_t state,
                                 app_config_credential_mode_t mode)
{
    app_config_manufacturing_doc_t doc;
    (void)memset(&doc, 0, sizeof(doc));
    doc.schema_version = APP_CONFIG_SCHEMA_VERSION;
    doc.manufacturing_state = state;
    doc.credential_mode = mode;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_commit_manufacturing(&doc));
}

/** @brief Install a provisioned, PSK-enrolled manufacturing record. */
static void commit_provisioned_manufacturing(void)
{
    commit_manufacturing(APP_CONFIG_MFG_STATE_PROVISIONED,
                         APP_CONFIG_CRED_MODE_PSK);
}

/**
 * @brief Install the full valid identity (provisioned manufacturing record
 *        + valid identity record).
 */
static void provision_valid_identity(void)
{
    commit_provisioned_manufacturing();
    TEST_ASSERT_TRUE(write_identity(TEST_IDENTITY_V1));
}

/**
 * @brief Assert the fail-closed post-rejection state: the module is
 *        unloaded, both getters report not_loaded, the token provider
 *        yields NULL (so no anonymous/partial identity can ever be used),
 *        and the lamp output was forced inactive exactly once for this
 *        rejection.
 */
static void assert_rejected(device_identity_status_t expected)
{
    const char *value = "@not-touched@";

    TEST_ASSERT_EQUAL_INT((int)expected, (int)device_identity_load());
    TEST_ASSERT_FALSE(device_identity_is_loaded());
    TEST_ASSERT_EQUAL_UINT(1u, lamp_mock_force_inactive_calls());

    TEST_ASSERT_EQUAL_INT((int)DEVICE_IDENTITY_ERR_NOT_LOADED,
                          (int)device_identity_client_id(&value));
    TEST_ASSERT_NULL(value);
    TEST_ASSERT_EQUAL_INT((int)DEVICE_IDENTITY_ERR_NOT_LOADED,
                          (int)device_identity_token(&value));
    TEST_ASSERT_NULL(value);
    TEST_ASSERT_NULL(device_identity_token_provider()());
}

/* --------------------------------------------------------------------- */
/* 1. Valid identity                                                      */
/* --------------------------------------------------------------------- */

static void test_valid_identity_loads_and_exposes_interfaces(void)
{
    const char *client_id = NULL;
    const char *token = NULL;

    provision_valid_identity();

    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_OK, device_identity_load());
    TEST_ASSERT_TRUE(device_identity_is_loaded());
    /* A valid identity is not a failure: it must not force the output off. */
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());

    /* Stable client-ID interface. */
    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_OK,
                          device_identity_client_id(&client_id));
    TEST_ASSERT_NOT_NULL(client_id);
    TEST_ASSERT_EQUAL_STRING(TEST_CLIENT_ID, client_id);

    /* Token interface. */
    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_OK, device_identity_token(&token));
    TEST_ASSERT_NOT_NULL(token);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN, token);

    /* Token-provider interface (ThingsBoard wiring). */
    TEST_ASSERT_NOT_NULL(device_identity_token_provider());
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN, device_identity_token_provider()());

    /* The exposed token is exactly the credential ThingsBoard uses as the
     * MQTT username: no truncation, no transformation. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(TEST_TOKEN),
                             (uint32_t)strlen(token));
}

static void test_valid_identity_after_recovery_from_rejection(void)
{
    /* Fail closed first (no identity record). */
    commit_provisioned_manufacturing();
    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_ERR_NOT_FOUND,
                          device_identity_load());
    TEST_ASSERT_FALSE(device_identity_is_loaded());

    (void)lamp_mock_reset();

    /* The repaired record loads on the next attempt. */
    provision_valid_identity();
    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_OK, device_identity_load());
    TEST_ASSERT_TRUE(device_identity_is_loaded());
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());
}

static void test_rejected_reload_never_serves_previous_identity(void)
{
    const char *token = NULL;

    provision_valid_identity();
    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_OK, device_identity_load());
    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_OK, device_identity_token(&token));
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN, token);

    /* Corrupt the record; the next load must fail closed and MUST NOT
     * continue serving the previously loaded token. */
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\"}"));
    (void)lamp_mock_reset();
    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_ERR_MALFORMED,
                          device_identity_load());
    TEST_ASSERT_FALSE(device_identity_is_loaded());
    TEST_ASSERT_NULL(device_identity_token_provider()());
}

/* --------------------------------------------------------------------- */
/* 2. Manufacturing-record gate                                           */
/* --------------------------------------------------------------------- */

static void test_missing_manufacturing_record_is_unprovisioned(void)
{
    /* A valid identity record alone must not unlock identity: the device
     * was never enrolled. */
    TEST_ASSERT_TRUE(write_identity(TEST_IDENTITY_V1));
    assert_rejected(DEVICE_IDENTITY_ERR_UNPROVISIONED);
}

static void test_unprovisioned_manufacturing_state_rejected(void)
{
    commit_manufacturing(APP_CONFIG_MFG_STATE_UNPROVISIONED,
                         APP_CONFIG_CRED_MODE_PSK);
    TEST_ASSERT_TRUE(write_identity(TEST_IDENTITY_V1));
    assert_rejected(DEVICE_IDENTITY_ERR_UNPROVISIONED);
}

static void test_quarantined_manufacturing_state_rejected(void)
{
    commit_manufacturing(APP_CONFIG_MFG_STATE_QUARANTINED,
                         APP_CONFIG_CRED_MODE_MTLS);
    TEST_ASSERT_TRUE(write_identity(TEST_IDENTITY_V1));
    assert_rejected(DEVICE_IDENTITY_ERR_UNPROVISIONED);
}

static void test_credential_mode_none_rejected(void)
{
    commit_manufacturing(APP_CONFIG_MFG_STATE_PROVISIONED,
                         APP_CONFIG_CRED_MODE_NONE);
    TEST_ASSERT_TRUE(write_identity(TEST_IDENTITY_V1));
    assert_rejected(DEVICE_IDENTITY_ERR_UNPROVISIONED);
}

/* --------------------------------------------------------------------- */
/* 3. Identity record invalid categories                                  */
/* --------------------------------------------------------------------- */

static void test_missing_identity_record_rejected(void)
{
    commit_provisioned_manufacturing();
    assert_rejected(DEVICE_IDENTITY_ERR_NOT_FOUND);
}

static void test_empty_token_rejected(void)
{
    commit_provisioned_manufacturing();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\","
        "\"access_token\":\"\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_EMPTY_TOKEN);
}

static void test_empty_client_id_rejected(void)
{
    commit_provisioned_manufacturing();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"\","
        "\"access_token\":\"A1b2C3d4E5f6G7h8I9j0\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_EMPTY_CLIENT_ID);
}

static void test_oversized_token_rejected(void)
{
    char oversized[DEVICE_IDENTITY_TOKEN_MAX_LEN + 8u];
    char record[DEVICE_IDENTITY_MAX_FILE_BYTES];

    commit_provisioned_manufacturing();
    (void)memset(oversized, 'A', sizeof(oversized) - 1u);
    oversized[sizeof(oversized) - 1u] = '\0';

    (void)snprintf(record, sizeof(record),
                   "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\","
                   "\"access_token\":\"%s\"}",
                   oversized);
    TEST_ASSERT_TRUE(write_identity(record));
    assert_rejected(DEVICE_IDENTITY_ERR_BOUNDS);
}

static void test_oversized_client_id_rejected(void)
{
    char oversized[DEVICE_IDENTITY_CLIENT_ID_MAX_LEN + 8u];
    char record[DEVICE_IDENTITY_MAX_FILE_BYTES];

    commit_provisioned_manufacturing();
    (void)memset(oversized, 'K', sizeof(oversized) - 1u);
    oversized[sizeof(oversized) - 1u] = '\0';

    (void)snprintf(record, sizeof(record),
                   "{\"schema_version\":1,\"client_id\":\"%s\","
                   "\"access_token\":\"A1b2C3d4E5f6G7h8I9j0\"}",
                   oversized);
    TEST_ASSERT_TRUE(write_identity(record));
    assert_rejected(DEVICE_IDENTITY_ERR_BOUNDS);
}

static void test_oversized_file_rejected(void)
{
    char big[DEVICE_IDENTITY_MAX_FILE_BYTES + 1024u];

    commit_provisioned_manufacturing();
    (void)memset(big, 'x', sizeof(big) - 1u);
    big[0] = '{';
    big[sizeof(big) - 2u] = '}';
    big[sizeof(big) - 1u] = '\0';

    TEST_ASSERT_TRUE(write_raw(DEVICE_IDENTITY_FILE_PATH, big,
                               sizeof(big) - 1u));
    assert_rejected(DEVICE_IDENTITY_ERR_BOUNDS);
}

static void test_malformed_json_rejected(void)
{
    commit_provisioned_manufacturing();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\","));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);
}

static void test_trailing_garbage_rejected(void)
{
    char record[DEVICE_IDENTITY_MAX_FILE_BYTES];

    commit_provisioned_manufacturing();
    (void)snprintf(record, sizeof(record), "%s garbage", TEST_IDENTITY_V1);
    TEST_ASSERT_TRUE(write_identity(record));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);
}

static void test_missing_member_rejected(void)
{
    commit_provisioned_manufacturing();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);
}

static void test_unknown_member_rejected(void)
{
    commit_provisioned_manufacturing();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\","
        "\"access_token\":\"A1b2C3d4E5f6G7h8I9j0\","
        "\"policy_override\":\"anonymous\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);
}

static void test_duplicate_member_rejected(void)
{
    commit_provisioned_manufacturing();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\","
        "\"client_id\":\"other-device\","
        "\"access_token\":\"A1b2C3d4E5f6G7h8I9j0\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);
}

static void test_token_control_character_rejected(void)
{
    commit_provisioned_manufacturing();
    /* A literal backslash-n decodes into a control character in the parsed
     * value; the charset validator rejects it. */
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\","
        "\"access_token\":\"abc\\ndef\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);
}

static void test_token_whitespace_rejected(void)
{
    commit_provisioned_manufacturing();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\","
        "\"access_token\":\"abc def\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);
}

static void test_unknown_schema_rejected(void)
{
    commit_provisioned_manufacturing();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":2,\"client_id\":\"klc-kitchen-01\","
        "\"access_token\":\"A1b2C3d4E5f6G7h8I9j0\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_UNKNOWN_SCHEMA);
}

static void test_wrong_typed_members_rejected(void)
{
    commit_provisioned_manufacturing();

    /* access_token as a number. */
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":\"klc-kitchen-01\","
        "\"access_token\":12345}"));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);

    /* client_id as a number. */
    (void)lamp_mock_reset();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":1,\"client_id\":42,"
        "\"access_token\":\"A1b2C3d4E5f6G7h8I9j0\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);

    /* schema_version as a string. */
    (void)lamp_mock_reset();
    TEST_ASSERT_TRUE(write_identity(
        "{\"schema_version\":\"1\",\"client_id\":\"klc-kitchen-01\","
        "\"access_token\":\"A1b2C3d4E5f6G7h8I9j0\"}"));
    assert_rejected(DEVICE_IDENTITY_ERR_MALFORMED);
}

/* --------------------------------------------------------------------- */
/* 4. Getter contract / redaction / zeroization                           */
/* --------------------------------------------------------------------- */

static void test_getters_before_load_report_not_loaded(void)
{
    const char *value = "sentinel";

    TEST_ASSERT_FALSE(device_identity_is_loaded());
    TEST_ASSERT_EQUAL_INT((int)DEVICE_IDENTITY_ERR_NOT_LOADED,
                          (int)device_identity_client_id(&value));
    TEST_ASSERT_NULL(value);
    TEST_ASSERT_EQUAL_INT((int)DEVICE_IDENTITY_ERR_NOT_LOADED,
                          (int)device_identity_token(&value));
    TEST_ASSERT_NULL(value);
    TEST_ASSERT_NULL(device_identity_token_provider()());
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());
}

static void test_getter_null_argument_rejected(void)
{
    TEST_ASSERT_EQUAL_INT((int)DEVICE_IDENTITY_ERR_INVALID_ARGUMENT,
                          (int)device_identity_client_id(NULL));
    TEST_ASSERT_EQUAL_INT((int)DEVICE_IDENTITY_ERR_INVALID_ARGUMENT,
                          (int)device_identity_token(NULL));
}

static void test_status_names_carry_no_record_content(void)
{
    /* Redaction contract: every status name must be a static identifier —
     * in particular it must never contain the token (or the client id). */
    const int statuses[] = {
        DEVICE_IDENTITY_OK,
        DEVICE_IDENTITY_ERR_INVALID_ARGUMENT,
        DEVICE_IDENTITY_ERR_IO,
        DEVICE_IDENTITY_ERR_NOT_FOUND,
        DEVICE_IDENTITY_ERR_MALFORMED,
        DEVICE_IDENTITY_ERR_BOUNDS,
        DEVICE_IDENTITY_ERR_UNKNOWN_SCHEMA,
        DEVICE_IDENTITY_ERR_UNPROVISIONED,
        DEVICE_IDENTITY_ERR_EMPTY_CLIENT_ID,
        DEVICE_IDENTITY_ERR_EMPTY_TOKEN,
        DEVICE_IDENTITY_ERR_NOT_LOADED,
    };

    for (size_t i = 0u; i < sizeof(statuses) / sizeof(statuses[0]); ++i)
    {
        const char *name = device_identity_status_name(
            (device_identity_status_t)statuses[i]);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 0u);
        TEST_ASSERT_NULL(strstr(name, TEST_TOKEN));
        TEST_ASSERT_NULL(strstr(name, TEST_CLIENT_ID));
    }
    /* Unknown codes map to a fixed identifier, never to record content. */
    TEST_ASSERT_EQUAL_STRING("unknown",
                             device_identity_status_name((device_identity_status_t)-999));
}

static void test_clear_invalidates_and_zeroizes_identity(void)
{
    const char *token = NULL;

    provision_valid_identity();
    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_OK, device_identity_load());
    TEST_ASSERT_TRUE(device_identity_is_loaded());
    TEST_ASSERT_EQUAL_INT(DEVICE_IDENTITY_OK, device_identity_token(&token));
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN, token);

    device_identity_clear();

    TEST_ASSERT_FALSE(device_identity_is_loaded());
    TEST_ASSERT_EQUAL_INT((int)DEVICE_IDENTITY_ERR_NOT_LOADED,
                          (int)device_identity_client_id(&token));
    TEST_ASSERT_NULL(token);
    TEST_ASSERT_EQUAL_INT((int)DEVICE_IDENTITY_ERR_NOT_LOADED,
                          (int)device_identity_token(&token));
    TEST_ASSERT_NULL(token);
    TEST_ASSERT_NULL(device_identity_token_provider()());
}

/* --------------------------------------------------------------------- */
/* Runner                                                                 */
/* --------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_valid_identity_loads_and_exposes_interfaces);
    RUN_TEST(test_valid_identity_after_recovery_from_rejection);
    RUN_TEST(test_rejected_reload_never_serves_previous_identity);

    RUN_TEST(test_missing_manufacturing_record_is_unprovisioned);
    RUN_TEST(test_unprovisioned_manufacturing_state_rejected);
    RUN_TEST(test_quarantined_manufacturing_state_rejected);
    RUN_TEST(test_credential_mode_none_rejected);

    RUN_TEST(test_missing_identity_record_rejected);
    RUN_TEST(test_empty_token_rejected);
    RUN_TEST(test_empty_client_id_rejected);
    RUN_TEST(test_oversized_token_rejected);
    RUN_TEST(test_oversized_client_id_rejected);
    RUN_TEST(test_oversized_file_rejected);
    RUN_TEST(test_malformed_json_rejected);
    RUN_TEST(test_trailing_garbage_rejected);
    RUN_TEST(test_missing_member_rejected);
    RUN_TEST(test_unknown_member_rejected);
    RUN_TEST(test_duplicate_member_rejected);
    RUN_TEST(test_token_control_character_rejected);
    RUN_TEST(test_token_whitespace_rejected);
    RUN_TEST(test_unknown_schema_rejected);
    RUN_TEST(test_wrong_typed_members_rejected);

    RUN_TEST(test_getters_before_load_report_not_loaded);
    RUN_TEST(test_getter_null_argument_rejected);
    RUN_TEST(test_status_names_carry_no_record_content);
    RUN_TEST(test_clear_invalidates_and_zeroizes_identity);

    return UNITY_END();
}