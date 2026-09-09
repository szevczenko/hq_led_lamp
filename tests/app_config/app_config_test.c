/**
 * @file app_config_test.c
 * @brief Host unit tests for the versioned product configuration service
 *        (TASK-108)
 *
 * Runs the production components/app_config code against the in-memory
 * OSAL file double (osal_file_mock.c) under Unity.  Coverage per the
 * TASK-108 definition of done:
 *
 *   1. success — commit writes the live document through the safe path and
 *      load round-trips it; a second commit refreshes the last-known-good
 *      copy from the previously valid live document,
 *   2. rejection — missing, truncated, oversized, malformed and
 *      unknown-schema documents never replace valid live configuration,
 *   3. atomicity — an interrupted write (fault injection) aborts the
 *      commit, leaves the live file untouched and cleans up the temporary
 *      file; a backend without atomic rename refuses the commit
 *      (APP_CONFIG_ERR_UNSUPPORTED) instead of copying non-atomically,
 *   4. recovery — after live-file corruption the validated last-known-good
 *      copy restores the live document (APP_CONFIG_OK_RECOVERED); a corrupt
 *      backup is never promoted, and corrupt live + corrupt good yields the
 *      original error,
 *   5. migration — explicit entry point migrates an old-schema document
 *      through the registered hook; unknown/newer schemas are rejected,
 *   6. redaction — secret-bearing keys never reach diagnostics output,
 *   7. no lamp state — the service exposes no write path for runtime state;
 *      invalid document kinds are rejected.
 */

#include <string.h>

#include "app_config.h"
#include "osal_error.h"
#include "osal_file_mock.h"

#include "unity.h"

/* --------------------------------------------------------------------- */
/* Fixtures                                                               */
/* --------------------------------------------------------------------- */

/** @brief A valid device document serialized at the current schema. */
static const char *const DEVICE_V1 =
    "{\"schema_version\":1,\"product\":\"HQ Lamp\","
    "\"hardware_revision\":\"B\",\"serial\":\"KLC-2024-000001\","
    "\"thingsboard_name\":\"klc-kitchen-01\"}";

/** @brief A valid manufacturing document serialized at the current schema. */
static const char *const MANUFACTURING_V1 =
    "{\"schema_version\":1,\"manufacturing_state\":1,\"credential_mode\":2}";

static void fill_device_doc(app_config_device_doc_t *doc)
{
    (void)memset(doc, 0, sizeof(*doc));
    doc->schema_version = APP_CONFIG_SCHEMA_VERSION;
    strncpy(doc->product, "HQ Lamp", sizeof(doc->product) - 1U);
    strncpy(doc->hardware_revision, "B",
            sizeof(doc->hardware_revision) - 1U);
    strncpy(doc->serial, "KLC-2024-000001", sizeof(doc->serial) - 1U);
    strncpy(doc->thingsboard_name, "klc-kitchen-01",
            sizeof(doc->thingsboard_name) - 1U);
}

static void fill_manufacturing_doc(app_config_manufacturing_doc_t *doc)
{
    (void)memset(doc, 0, sizeof(*doc));
    doc->schema_version      = APP_CONFIG_SCHEMA_VERSION;
    doc->manufacturing_state = APP_CONFIG_MFG_STATE_PROVISIONED;
    doc->credential_mode     = APP_CONFIG_CRED_MODE_MTLS;
}

/** @brief Count non-overlapping occurrences of @p needle in @p haystack. */
static int count_occurrences(const char *haystack, const char *needle)
{
    int count = 0;
    const size_t needle_len = strlen(needle);
    while ((haystack = strstr(haystack, needle)) != NULL)
    {
        ++count;
        haystack += needle_len;
    }
    return count;
}

void setUp(void)
{
    osal_file_mock_reset();
    app_config_register_migration_handler(NULL);
}

void tearDown(void)
{
}

/* --------------------------------------------------------------------- */
/* 1. Success path                                                        */
/* --------------------------------------------------------------------- */

static void test_commit_then_load_roundtrip_device(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* The live document exists; no backup yet (nothing to back up before
     * the first commit). */
    TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(
        APP_CONFIG_DEVICE_PATH ".good"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_UINT32(APP_CONFIG_SCHEMA_VERSION,
                             loaded.schema_version);
    TEST_ASSERT_EQUAL_STRING("HQ Lamp", loaded.product);
    TEST_ASSERT_EQUAL_STRING("B", loaded.hardware_revision);
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);
    TEST_ASSERT_EQUAL_STRING("klc-kitchen-01", loaded.thingsboard_name);
}

static void test_second_commit_refreshes_last_known_good(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));

    /* Second commit: the previous live content is archived into .good and
     * validated there before the live file is replaced. */
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char good[512];
    TEST_ASSERT_TRUE(osal_file_mock_has_file(
        APP_CONFIG_DEVICE_PATH ".good"));
    TEST_ASSERT_TRUE(osal_file_mock_get_file(
        APP_CONFIG_DEVICE_PATH ".good", good, sizeof(good)));
    TEST_ASSERT_EQUAL_STRING(live, good);

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000002", loaded.serial);
}

static void test_commit_then_load_roundtrip_manufacturing(void)
{
    app_config_manufacturing_doc_t doc;
    fill_manufacturing_doc(&doc);

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_commit_manufacturing(&doc));

    /* The stored bytes match the canonical manufacturing document. */
    char raw[256];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_MANUFACTURING_PATH,
                                             raw, sizeof(raw)));
    TEST_ASSERT_EQUAL_STRING(MANUFACTURING_V1, raw);

    app_config_manufacturing_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_load_manufacturing(&loaded));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_MFG_STATE_PROVISIONED,
                          loaded.manufacturing_state);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_CRED_MODE_MTLS, loaded.credential_mode);
}

/* --------------------------------------------------------------------- */
/* 2. Rejection rules                                                     */
/* --------------------------------------------------------------------- */

static void test_missing_document_reports_not_found(void)
{
    app_config_device_doc_t doc;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_NOT_FOUND,
                          app_config_load_device(&doc));
}

static void test_truncated_json_is_rejected(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Corrupt the live document in place (truncated). */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":1,\"product\":\"HQ Lam"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));
}

static void test_oversized_field_is_rejected(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));

    /* serial bound is APP_CONFIG_SERIAL_MAX_LEN; 40 characters exceed it.
     * Exercised through the serialized form (a commit request cannot carry
     * an over-long field because the document struct enforces the bound). */
    static char bad[256];
    (void)snprintf(bad, sizeof(bad),
                   "{\"schema_version\":1,\"product\":\"HQ Lamp\","
                   "\"hardware_revision\":\"B\",\"serial\":"
                   "\"7777777777777777777777777777777777777777\","
                   "\"thingsboard_name\":\"klc-kitchen-01\"}");

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_BOUNDS,
                          app_config_validate_device_json(bad, NULL));

    /* Invalid data must not replace the valid live configuration. */
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);

    char after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, after,
                                             sizeof(after)));
    TEST_ASSERT_EQUAL_STRING(live, after);
}

static void test_oversized_file_is_rejected(void)
{
    /* > APP_CONFIG_MAX_FILE_BYTES of syntactically valid JSON. */
    static char big[APP_CONFIG_MAX_FILE_BYTES + 64U];
    const size_t fixed = (size_t)snprintf(
        big, sizeof(big),
        "{\"schema_version\":1,\"product\":\"");
    for (size_t i = fixed; i < (sizeof(big) - 3U); ++i)
    {
        big[i] = 'x';
    }
    big[sizeof(big) - 3U] = '"';
    big[sizeof(big) - 2U] = '}';
    big[sizeof(big) - 1U] = '\0';

    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH, big));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_BOUNDS,
                          app_config_load_device(&loaded));
}

static void test_file_exactly_at_size_bound_is_accepted(void)
{
    /* Build a syntactically AND semantically valid document whose total
     * text length is exactly APP_CONFIG_MAX_FILE_BYTES: JSON whitespace
     * after the opening brace pads the document to the advertised maximum.
     * Only sizes beyond the bound are a bounds violation. */
    static const char *const BASE =
        "{\"schema_version\":1,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-2024-000001\","
        "\"thingsboard_name\":\"klc-kitchen-01\"}";
    static char exact[APP_CONFIG_MAX_FILE_BYTES + 1U];
    const size_t base_len = strlen(BASE);
    TEST_ASSERT_TRUE(base_len < APP_CONFIG_MAX_FILE_BYTES);
    const size_t pad = APP_CONFIG_MAX_FILE_BYTES - base_len;

    exact[0] = '{';
    for (size_t i = 0U; i < pad; ++i)
    {
        exact[1U + i] = ' ';
    }
    memcpy(&exact[1U + pad], &BASE[1], base_len); /* skip BASE's '{' */
    exact[APP_CONFIG_MAX_FILE_BYTES] = '\0';
    TEST_ASSERT_EQUAL_UINT32(APP_CONFIG_MAX_FILE_BYTES,
                             (uint32_t)strlen(exact));

    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH, exact));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);
}

static void test_empty_identity_fields_are_rejected(void)
{
    /* All identity fields are defined as 1..N characters: an empty string
     * is a field-bounds error, not a valid value. */
    static const char *const DOCS[] = {
        "{\"schema_version\":1,\"product\":\"\","
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"klc-01\"}",
        "{\"schema_version\":1,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"klc-01\"}",
        "{\"schema_version\":1,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"\","
        "\"thingsboard_name\":\"klc-01\"}",
        "{\"schema_version\":1,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"\"}",
    };

    for (size_t i = 0U; i < (sizeof(DOCS) / sizeof(DOCS[0])); ++i)
    {
        TEST_ASSERT_EQUAL_INT_MESSAGE(APP_CONFIG_ERR_BOUNDS,
                                      app_config_validate_device_json(
                                          DOCS[i], NULL),
                                      DOCS[i]);
    }
}

static void test_schema_version_above_u32_range_is_unknown_schema(void)
{
    /* An integral version beyond the uint32_t capacity is still just a
     * newer (unsupported) schema, not a field-bounds violation. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":10000000000,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"klc-01\"}"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_UNKNOWN_SCHEMA,
                          app_config_load_device(&loaded));
}

static void test_non_finite_schema_version_is_malformed(void)
{
    /* Non-finite numbers ("1e309" parses as +Infinity) must be rejected
     * before any floating-to-integer conversion (which would be undefined
     * behavior for out-of-range operands). */
    app_config_device_doc_t loaded;
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":1e309,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"klc-01\"}"));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));

    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":-1e309,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"klc-01\"}"));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));
}

static void test_oversized_integral_version_is_unknown_schema(void)
{
    /* Finite integral values beyond the exact double integer range are a
     * newer (unsupported) schema, never undefined behavior. */
    app_config_manufacturing_doc_t loaded;
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_MANUFACTURING_PATH,
        "{\"schema_version\":123456789012345678901234567890,"
        "\"manufacturing_state\":1,\"credential_mode\":2}"));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_UNKNOWN_SCHEMA,
                          app_config_load_manufacturing(&loaded));
}

static void test_non_finite_bounded_number_is_rejected(void)
{
    app_config_manufacturing_doc_t loaded;
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_MANUFACTURING_PATH,
        "{\"schema_version\":1,\"manufacturing_state\":1e309,"
        "\"credential_mode\":2}"));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_manufacturing(&loaded));
}

static void test_commit_rejects_unterminated_typed_strings(void)
{
    /* A caller-controlled typed document whose string arrays are not
     * NUL-terminated must be rejected with a bounded scan (no strlen /
     * read past the array) and must never replace live configuration. */
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));

    /* Overwrite the whole struct (no terminator anywhere) with a valid
     * charset character, so an unbounded strlen would run off the array. */
    (void)memset(doc.product, 'P', sizeof(doc.product));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_BOUNDS,
                          app_config_commit_device(&doc));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".tmp"));

    /* Charset violation: also rejected, live file untouched. */
    fill_device_doc(&doc);
    (void)memset(doc.serial, 'x', sizeof(doc.serial));
    doc.serial[sizeof(doc.serial) - 1U] = '\0';
    doc.serial[0] = '!'; /* not in the serial charset */
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_commit_device(&doc));

    /* The previously committed valid document is unchanged. */
    char live_now[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live_now,
                                             sizeof(live_now)));
    TEST_ASSERT_EQUAL_STRING(live, live_now);
}

static void test_unknown_schema_version_is_rejected(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":99,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"klc-01\"}"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_UNKNOWN_SCHEMA,
                          app_config_load_device(&loaded));
}

static void test_missing_schema_version_is_malformed(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"product\":\"HQ Lamp\",\"hardware_revision\":\"B\","
        "\"serial\":\"KLC-1\",\"thingsboard_name\":\"klc-01\"}"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));
}

static void test_unknown_member_is_rejected(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":1,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"klc-01\",\"token\":\"s3cr3t\"}"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));

    /* The smuggled secret never made it into a managed document. */
    TEST_ASSERT_FALSE(osal_file_mock_has_file(
        APP_CONFIG_DEVICE_PATH ".good"));
}

static void test_wrong_typed_field_is_rejected(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":1,\"product\":7,"
        "\"hardware_revision\":\"B\",\"serial\":\"KLC-1\","
        "\"thingsboard_name\":\"klc-01\"}"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));
}

static void test_bad_serial_charset_is_rejected(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":1,\"product\":\"HQ Lamp\","
        "\"hardware_revision\":\"B\",\"serial\":\"bad serial!\","
        "\"thingsboard_name\":\"klc-01\"}"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));
}

static void test_invalid_manufacturing_state_is_rejected(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_MANUFACTURING_PATH,
        "{\"schema_version\":1,\"manufacturing_state\":7,"
        "\"credential_mode\":2}"));

    app_config_manufacturing_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_BOUNDS,
                          app_config_load_manufacturing(&loaded));
}

static void test_invalid_commit_arguments_are_rejected(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    doc.schema_version = APP_CONFIG_SCHEMA_VERSION + 1U;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_UNKNOWN_SCHEMA,
                          app_config_commit_device(&doc));

    fill_device_doc(&doc);
    doc.schema_version = 0U;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MIGRATION,
                          app_config_commit_device(&doc));

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_INVALID_ARGUMENT,
                          app_config_commit_device(NULL));
}

static void test_invalid_doc_kind_is_rejected(void)
{
    TEST_ASSERT_NULL(app_config_doc_path((app_config_doc_kind_t)42));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_INVALID_ARGUMENT,
                          app_config_migrate_stored((app_config_doc_kind_t)42));
}

/* --------------------------------------------------------------------- */
/* 3. Atomicity / interrupted write                                       */
/* --------------------------------------------------------------------- */

static void test_interrupted_write_leaves_live_document_untouched(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));

    /* Interrupt the next commit mid-write: the temporary file is left
     * truncated, the live document and its backup are untouched. */
    osal_file_mock_fail_write_after(10);
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_commit_device(&doc));

    char after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, after,
                                             sizeof(after)));
    TEST_ASSERT_EQUAL_STRING(live, after);

    /* The temporary file is cleaned up on a handled failure. */
    TEST_ASSERT_FALSE(osal_file_mock_has_file(
        APP_CONFIG_DEVICE_PATH ".tmp"));

    /* The still-valid live document loads normally. */
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);
}

static void test_load_ignores_a_stale_interrupted_temporary_file(void)
{
    /* A power loss during a commit left a truncated ".tmp" behind; the live
     * document is intact and load must not be confused by the leftover. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH, DEVICE_V1));
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH ".tmp", "{\"schema_version\":1,\"prod"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("klc-kitchen-01", loaded.thingsboard_name);
}

static void test_rename_unsupported_refuses_commit(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));

    /* Backend without atomic rename: the commit is refused.  A non-atomic
     * copy-then-remove replacement could truncate the valid live document
     * if interrupted, so it is never attempted; the live file (and its
     * last-known-good copy, if any) must remain exactly as they were. */
    osal_file_mock_set_rename_status(OSAL_ERR_OPERATION_NOT_SUPPORTED);

    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_UNSUPPORTED,
                          app_config_commit_device(&doc));

    char after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, after,
                                             sizeof(after)));
    TEST_ASSERT_EQUAL_STRING(live, after);
    TEST_ASSERT_FALSE(osal_file_mock_has_file(
        APP_CONFIG_DEVICE_PATH ".tmp"));

    /* The refused document was not stored anywhere. */
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);

    osal_file_mock_set_rename_status(OSAL_SUCCESS);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000002", loaded.serial);
}

static void test_backup_staging_failure_preserves_last_known_good(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Produce a validated backup with a second commit. */
    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char good_before[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_before, sizeof(good_before)));
    TEST_ASSERT_EQUAL_STRING(live, good_before);

    /* Model the interrupted staging copy: the ".good" file becomes a truncated
     * document (exactly what a mid-copy osal_cp() failure leaves behind) and
     * the next staging osal_cp() is refused outright, so the commit cannot
     * repair the backup either. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH ".good",
                                             "{\"schema_"));
    osal_file_mock_fail_next_cp(true);
    strncpy(doc.serial, "KLC-2024-000003", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_commit_device(&doc));

    /* The truncated backup survives the aborted commit untouched: the
     * staging copy failed before the previous "<live>.good" was replaced,
     * and the component never repairs a backup non-atomically. */
    char good_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(
        APP_CONFIG_DEVICE_PATH ".good", good_after, sizeof(good_after)));
    TEST_ASSERT_EQUAL_STRING("{\"schema_", good_after);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_validate_device_json(good_after, NULL));

    /* The failed commit left the live document untouched (the staging copy
     * failed before the live file was ever replaced): it still holds the
     * 000002 document from the second commit. */
    char after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, after,
                                             sizeof(after)));
    TEST_ASSERT_NOT_NULL(strstr(after, "KLC-2024-000002"));
    TEST_ASSERT_NULL(strstr(after, "KLC-2024-000003"));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_validate_device_json(after, NULL));

    /* A retry without the fault succeeds: the new commit archives the valid
     * live (000002) document into ".good", replacing the corrupt remnant,
     * and refreshes the live file to 000003. */
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* The backup now holds the previously live (000002) document, while the
     * live file carries the new (000003) content. */
    char live_before_retry[512];
    TEST_ASSERT_TRUE(strncpy(live_before_retry, after, sizeof(live_before_retry)) != NULL);

    char good_retry[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, after,
                                             sizeof(after)));
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_retry, sizeof(good_retry)));
    TEST_ASSERT_EQUAL_STRING(live_before_retry, good_retry);
    TEST_ASSERT_NULL(strstr(good_retry, "KLC-2024-000003"));
    TEST_ASSERT_NULL(strstr(after, "KLC-2024-000002"));
    TEST_ASSERT_NOT_NULL(strstr(after, "KLC-2024-000003"));
}

static void test_rename_failure_leaves_live_document_untouched(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Produce a validated backup with a second commit. */
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));

    char good_before[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_before,
                                             sizeof(good_before)));
    /* The backup holds the previously live (000001) document. */
    TEST_ASSERT_NULL(strstr(good_before, "KLC-2024-000002"));

    osal_file_mock_set_rename_status(OSAL_ERR_OPERATION_NOT_SUPPORTED);
    strncpy(doc.serial, "KLC-2024-000003", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_UNSUPPORTED,
                          app_config_commit_device(&doc));

    /* The last-known-good copy also survived the aborted commit. */
    char good_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_after, sizeof(good_after)));
    /* The backup never picked up the refused document and still holds a
     * valid last-known-good document (the staging rename was refused with
     * ERR_UNSUPPORTED after this commit's live rename already failed, so
     * the previous backup is preserved as-is). */
    TEST_ASSERT_NULL(strstr(good_after, "KLC-2024-000003"));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_validate_device_json(good_after, NULL));
    /* And the still-valid live document loads normally. */
    app_config_device_doc_t reloaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&reloaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000002", reloaded.serial);
}

/* --------------------------------------------------------------------- */
/* 4. Corruption and last-known-good recovery                             */
/* --------------------------------------------------------------------- */

static void test_recovery_from_last_known_good(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Produce a last-known-good copy by committing a second version. */
    char first[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, first,
                                             sizeof(first)));
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Corrupt the live document after the fact (bit rot, truncated write
     * by an external actor, ...). */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH,
                                             "{\"corrupt"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK_RECOVERED,
                          app_config_load_device(&loaded));

    /* The recovered data is the previous valid document, nothing else. */
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);
    TEST_ASSERT_EQUAL_STRING("klc-kitchen-01", loaded.thingsboard_name);

    /* The live file was restored from the validated backup. */
    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));
    TEST_ASSERT_EQUAL_STRING(first, live);
}

static void test_failed_recovery_leaves_doc_untouched(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Produce a validated backup with a second commit. */
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Corrupt the live document so the load falls into the recovery path,
     * then make the restore itself fail (rename refused by the backend). */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH,
                                             "{\"corrupt"));
    osal_file_mock_set_rename_status(OSAL_ERR_OPERATION_NOT_SUPPORTED);

    /* Poison the output document so any write during a failed recovery is
     * observable: on failure the caller's doc must stay untouched. */
    app_config_device_doc_t loaded;
    (void)memset(&loaded, 0xA5, sizeof(loaded));

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_UNSUPPORTED,
                          app_config_load_device(&loaded));

    /* The output was never populated: recovery published nothing. */
    TEST_ASSERT_EQUAL_HEX8(0xA5, ((const unsigned char *)&loaded)[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA5,
                           ((const unsigned char *)&loaded)[sizeof(loaded) - 1U]);

    /* Recovery consumed the one-shot rename fault. */
    osal_file_mock_set_rename_status(OSAL_SUCCESS);
    (void)memset(&loaded, 0, sizeof(loaded));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK_RECOVERED,
                          app_config_load_device(&loaded));
    /* The backup holds the previous (000001) document. */
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);
}

static void test_corrupt_backup_is_never_used(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Corrupt live AND last-known-good copies. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH,
                                             "???"));
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH ".good", "{\"half"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));

    /* No fabricated data: both files were left as they were. */
    char live[8];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));
    TEST_ASSERT_EQUAL_STRING("???", live);
}

static void test_corrupt_live_file_is_not_archived_as_good(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));

    /* Produce a valid backup with a second commit. */
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Corrupt the live document, then commit a new version: the commit
     * repairs the live file, and the stale backup must not be overwritten
     * with corrupt data. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH,
                                             "{nope"));
    strncpy(doc.serial, "KLC-2024-000003", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char good[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(
        APP_CONFIG_DEVICE_PATH ".good", good, sizeof(good)));

    /* The backup still holds the last valid document (the version from
     * before the corruption), never the corrupt bytes or the new content. */
    TEST_ASSERT_NOT_NULL(strstr(good, "KLC-2024-000001"));
    TEST_ASSERT_NULL(strstr(good, "nope"));
    TEST_ASSERT_NULL(strstr(good, "KLC-2024-000003"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000003", loaded.serial);
}

static void test_corrupt_backup_does_not_shadow_valid_live_document(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH,
                                             DEVICE_V1));
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH ".good", "garbage"));

    /* The live document is valid; a corrupt backup must not disturb it. */
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);
}

/* --------------------------------------------------------------------- */
/* 5. Migration                                                           */
/* --------------------------------------------------------------------- */

static int s_migration_calls;

static app_config_status_t migrate_v0_device(const char *json,
                                             uint32_t from_version,
                                             char *out,
                                             size_t out_cap)
{
    (void)json;
    (void)from_version;
    /* v0 used "device_name" for the ThingsBoard name and had no explicit
     * hardware revision. */
    (void)snprintf(out, out_cap,
                   "{\"schema_version\":1,\"product\":\"HQ Lamp\","
                   "\"hardware_revision\":\"A\",\"serial\":\"KLC-2023-9\","
                   "\"thingsboard_name\":\"klc-legacy\"}");
    ++s_migration_calls;
    return APP_CONFIG_OK;
}

static void test_migrate_stored_upgrades_legacy_document(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":0,\"product\":\"HQ Lamp\","
        "\"device_name\":\"klc-legacy\"}"));

    s_migration_calls = 0;
    app_config_register_migration_handler(migrate_v0_device);

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_migrate_stored(APP_CONFIG_DOC_DEVICE));
    TEST_ASSERT_EQUAL_INT(1, s_migration_calls);

    /* The migrated document was stored at the current schema. */
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_UINT32(APP_CONFIG_SCHEMA_VERSION,
                             loaded.schema_version);
    TEST_ASSERT_EQUAL_STRING("klc-legacy", loaded.thingsboard_name);
}

static void test_load_migrates_legacy_document(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":0,\"product\":\"HQ Lamp\","
        "\"device_name\":\"klc-legacy\"}"));

    s_migration_calls = 0;
    app_config_register_migration_handler(migrate_v0_device);

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_INT(1, s_migration_calls);
    TEST_ASSERT_EQUAL_STRING("klc-legacy", loaded.thingsboard_name);
}

static void test_migration_without_handler_fails(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":0,\"product\":\"HQ Lamp\","
        "\"device_name\":\"klc-legacy\"}"));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MIGRATION,
                          app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MIGRATION,
                          app_config_migrate_stored(APP_CONFIG_DOC_DEVICE));
}

static app_config_status_t migrate_v0_broken(const char *json,
                                             uint32_t from_version,
                                             char *out,
                                             size_t out_cap)
{
    (void)json;
    (void)from_version;
    (void)out_cap;
    /* Garbage output: must be rejected by re-validation, never stored. */
    (void)snprintf(out, APP_CONFIG_MAX_FILE_BYTES + 1U, "{oops");
    ++s_migration_calls;
    return APP_CONFIG_OK;
}

static void test_migration_with_invalid_handler_output_fails(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(
        APP_CONFIG_DEVICE_PATH,
        "{\"schema_version\":0,\"product\":\"HQ Lamp\","
        "\"device_name\":\"klc-legacy\"}"));

    /* Handler emits garbage: the service must reject it, not store it. */
    s_migration_calls = 0;
    app_config_register_migration_handler(migrate_v0_broken);

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MIGRATION,
                          app_config_migrate_stored(APP_CONFIG_DOC_DEVICE));
    TEST_ASSERT_EQUAL_INT(1, s_migration_calls);

    /* The corrupt "migration" never reached storage. */
    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"schema_version\":0,\"product\":\"HQ Lamp\","
        "\"device_name\":\"klc-legacy\"}",
        live);
}

static void test_migrate_stored_current_version_is_noop(void)
{
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH,
                                             DEVICE_V1));

    s_migration_calls = 0;
    app_config_register_migration_handler(migrate_v0_device);

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_migrate_stored(APP_CONFIG_DOC_DEVICE));
    TEST_ASSERT_EQUAL_INT(0, s_migration_calls);

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);
}

/* --------------------------------------------------------------------- */
/* 6. Secret redaction                                                    */
/* --------------------------------------------------------------------- */

static void test_redact_masks_secret_values(void)
{
    const char *doc =
        "{\"schema_version\":1,\"product\":\"HQ Lamp\","
        "\"token\":\"s3cr3t\",\"Password\":\"hunter2\","
        "\"nested\":{\"psk\":\"wifipass\",\"note\":\"visible\"}}";

    char out[512];
    const size_t n = app_config_redact(doc, out, sizeof(out));

    TEST_ASSERT_TRUE(n > 0U);
    TEST_ASSERT_NULL(strstr(out, "s3cr3t"));
    TEST_ASSERT_NULL(strstr(out, "hunter2"));
    TEST_ASSERT_NULL(strstr(out, "wifipass"));
    TEST_ASSERT_NOT_NULL(strstr(out, "[redacted]"));
    TEST_ASSERT_NOT_NULL(strstr(out, "HQ Lamp"));
    TEST_ASSERT_NOT_NULL(strstr(out, "visible"));
}

static void test_redact_handles_unparsable_input(void)
{
    char out[64];
    TEST_ASSERT_EQUAL_INT((int)strlen("[unparsable document]"),
                          (int)app_config_redact("{oops", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("[unparsable document]", out);

    TEST_ASSERT_EQUAL_INT(0, (int)app_config_redact(NULL, out, sizeof(out)));
}

static void test_redact_replaces_whole_secret_values(void)
{
    /* A secret-bearing key hides its ENTIRE value, whatever the value's
     * type: objects, arrays, numbers and booleans included. */
    const char *doc =
        "{\"password\":{\"value\":\"secret\",\"hint\":\"abc\"},"
        "\"token\":[\"one\",\"two\"],"
        "\"psk\":12345,"
        "\"key\":true,"
        "\"note\":\"visible\"}";

    char out[512];
    const size_t n = app_config_redact(doc, out, sizeof(out));

    TEST_ASSERT_TRUE(n > 0U);
    TEST_ASSERT_NULL(strstr(out, "secret"));
    TEST_ASSERT_NULL(strstr(out, "hint"));
    TEST_ASSERT_NULL(strstr(out, "abc"));
    TEST_ASSERT_NULL(strstr(out, "one"));
    TEST_ASSERT_NULL(strstr(out, "two"));
    TEST_ASSERT_NULL(strstr(out, "12345"));
    TEST_ASSERT_NULL(strstr(out, "true"));
    TEST_ASSERT_NOT_NULL(strstr(out, "visible"));
    /* Every secret-bearing member now holds exactly one redaction. */
    TEST_ASSERT_EQUAL_INT(4, count_occurrences(out, "[redacted]"));
}

/* --------------------------------------------------------------------- */
/* 7. No lamp state persistence                                           */
/* --------------------------------------------------------------------- */

static void test_only_product_documents_are_managed(void)
{
    /* The managed path set is exactly the two product documents; there is
     * no /state path and no write path for runtime lamp state. */
    TEST_ASSERT_EQUAL_STRING("/config/device.json",
                             app_config_doc_path(APP_CONFIG_DOC_DEVICE));
    TEST_ASSERT_EQUAL_STRING("/config/manufacturing.json",
                             app_config_doc_path(
                                 APP_CONFIG_DOC_MANUFACTURING));

    /* Committing with a wrong schema version is refused: runtime state
     * cannot be smuggled through the commit path either. */
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    doc.schema_version = APP_CONFIG_SCHEMA_VERSION + 1U;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_UNKNOWN_SCHEMA,
                          app_config_commit_device(&doc));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH));
}

/* --------------------------------------------------------------------- */
/* Runner                                                                 */
/* --------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    /* 1. Success */
    RUN_TEST(test_commit_then_load_roundtrip_device);
    RUN_TEST(test_second_commit_refreshes_last_known_good);
    RUN_TEST(test_commit_then_load_roundtrip_manufacturing);

    /* 2. Rejection */
    RUN_TEST(test_missing_document_reports_not_found);
    RUN_TEST(test_truncated_json_is_rejected);
    RUN_TEST(test_oversized_field_is_rejected);
    RUN_TEST(test_oversized_file_is_rejected);
    RUN_TEST(test_file_exactly_at_size_bound_is_accepted);
    RUN_TEST(test_empty_identity_fields_are_rejected);
    RUN_TEST(test_schema_version_above_u32_range_is_unknown_schema);
    RUN_TEST(test_non_finite_schema_version_is_malformed);
    RUN_TEST(test_oversized_integral_version_is_unknown_schema);
    RUN_TEST(test_non_finite_bounded_number_is_rejected);
    RUN_TEST(test_commit_rejects_unterminated_typed_strings);
    RUN_TEST(test_unknown_schema_version_is_rejected);
    RUN_TEST(test_missing_schema_version_is_malformed);
    RUN_TEST(test_unknown_member_is_rejected);
    RUN_TEST(test_wrong_typed_field_is_rejected);
    RUN_TEST(test_bad_serial_charset_is_rejected);
    RUN_TEST(test_invalid_manufacturing_state_is_rejected);
    RUN_TEST(test_invalid_commit_arguments_are_rejected);
    RUN_TEST(test_invalid_doc_kind_is_rejected);

    /* 3. Atomicity / interrupted write */
    RUN_TEST(test_interrupted_write_leaves_live_document_untouched);
    RUN_TEST(test_load_ignores_a_stale_interrupted_temporary_file);
    RUN_TEST(test_rename_unsupported_refuses_commit);
    RUN_TEST(test_rename_failure_leaves_live_document_untouched);
    RUN_TEST(test_backup_staging_failure_preserves_last_known_good);

    /* 4. Corruption / recovery */
    RUN_TEST(test_recovery_from_last_known_good);
    RUN_TEST(test_failed_recovery_leaves_doc_untouched);
    RUN_TEST(test_corrupt_backup_is_never_used);
    RUN_TEST(test_corrupt_live_file_is_not_archived_as_good);
    RUN_TEST(test_corrupt_backup_does_not_shadow_valid_live_document);

    /* 5. Migration */
    RUN_TEST(test_migrate_stored_upgrades_legacy_document);
    RUN_TEST(test_load_migrates_legacy_document);
    RUN_TEST(test_migration_without_handler_fails);
    RUN_TEST(test_migration_with_invalid_handler_output_fails);
    RUN_TEST(test_migrate_stored_current_version_is_noop);

    /* 6. Redaction */
    RUN_TEST(test_redact_masks_secret_values);
    RUN_TEST(test_redact_replaces_whole_secret_values);
    RUN_TEST(test_redact_handles_unparsable_input);

    /* 7. No lamp state */
    RUN_TEST(test_only_product_documents_are_managed);

    return UNITY_END();
}
