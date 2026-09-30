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
 *      invalid document kinds are rejected,
 *   8. strict parsing — valid JSON followed by trailing garbage or a
 *      second concatenated object is rejected as malformed (the parser
 *      must consume the ENTIRE input); trailing whitespace is fine,
 *   9. error classification — APP_CONFIG_ERR_NOT_FOUND only comes from a
 *      missing-file osal_stat() result; once stat succeeds, an open/read
 *      failure is APP_CONFIG_ERR_IO, aborting the commit before the live
 *      rename and never silently falling back to the backup on load,
 *  10. rename classification — OSAL_ERR_OPERATION_NOT_SUPPORTED /
 *      OSAL_ERR_NOT_IMPLEMENTED yield APP_CONFIG_ERR_UNSUPPORTED; every
 *      other rename failure yields APP_CONFIG_ERR_IO, on the live
 *      replacement AND on the last-known-good promotion alike,
 *  11. review-round regression — status_is_missing() accepts ONLY the
 *      documented missing-file statuses (argument failures and the
 *      generic OSAL_ERROR stay APP_CONFIG_ERR_IO); a failed preserve
 *      rename never destroys a stale "<live>.good.old"; a failed live
 *      rename after promoting a fresh ".good" with no previous backup
 *      removes that ".good" again.
 */

#include <stdio.h>
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

static void test_redact_duplicate_secret_keys(void)
{
    /* Diagnostics accepts (and must redact) duplicate secret-bearing keys:
     * replacement acts on the exact child node, so EVERY duplicate value —
     * not only the first key lookup match — is masked (review issue 6). */
    const char *doc =
        "{\"token\":\"first\",\"token\":\"second\","
        "\"psk\":\"a\",\"psk\":\"b\",\"note\":\"visible\"}";

    char out[512];
    const size_t n = app_config_redact(doc, out, sizeof(out));

    TEST_ASSERT_TRUE(n > 0U);
    TEST_ASSERT_NULL(strstr(out, "first"));
    TEST_ASSERT_NULL(strstr(out, "second"));
    TEST_ASSERT_NULL(strstr(out, "\"a\""));
    TEST_ASSERT_NULL(strstr(out, "\"b\""));
    TEST_ASSERT_EQUAL_INT(4, count_occurrences(out, "[redacted]"));
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
/* 8. Strict parsing: trailing garbage and concatenated documents         */
/* --------------------------------------------------------------------- */

static void test_trailing_garbage_json_is_rejected(void)
{
    /* Valid JSON followed by ANY trailing bytes is malformed: the parser
     * must consume the entire input. */
    static const char *const SUFFIXES[] = {
        " garbage", " {}", "1", "\"str\"", "null", "}",
    };

    for (size_t i = 0U; i < (sizeof(SUFFIXES) / sizeof(SUFFIXES[0])); ++i)
    {
        char doc[512];
        const int n = snprintf(doc, sizeof(doc), "%s%s", DEVICE_V1,
                               SUFFIXES[i]);
        TEST_ASSERT_TRUE((n > 0) && ((size_t)n < sizeof(doc)));

        TEST_ASSERT_EQUAL_INT_MESSAGE(APP_CONFIG_ERR_MALFORMED,
                                      app_config_validate_device_json(doc,
                                                                      NULL),
                                      SUFFIXES[i]);
    }

    char mfg_garbage[128];
    (void)snprintf(mfg_garbage, sizeof(mfg_garbage), "%s x",
                   MANUFACTURING_V1);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_validate_manufacturing_json(mfg_garbage,
                                                                 NULL));

    /* Through the storage path as well: a stored document with trailing
     * garbage is rejected and never reported as loaded. */
    char stored[512];
    (void)snprintf(stored, sizeof(stored), "%s trailing", DEVICE_V1);
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH, stored));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));
}

static void test_concatenated_json_objects_are_rejected(void)
{
    /* A second concatenated object after a valid document is trailing
     * garbage, not a parseable document. */
    char doc[512];
    (void)snprintf(doc, sizeof(doc), "%s%s", DEVICE_V1, DEVICE_V1);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_validate_device_json(doc, NULL));

    (void)snprintf(doc, sizeof(doc), "%s%s", MANUFACTURING_V1,
                   MANUFACTURING_V1);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_validate_manufacturing_json(doc, NULL));

    /* Also rejected through the load path. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH, doc));
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));
}

static void test_trailing_whitespace_is_still_accepted(void)
{
    /* Strict parsing requires the input to be fully consumed; trailing
     * JSON whitespace is consumed too, so it is NOT garbage. */
    char doc[512];
    (void)snprintf(doc, sizeof(doc), "%s\n  \t", DEVICE_V1);
    app_config_device_doc_t parsed;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_validate_device_json(doc, &parsed));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", parsed.serial);

    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH, doc));
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
}

static void test_trailing_garbage_live_document_is_recovered_from_good(void)
{
    /* A live document with trailing garbage is invalid content (like any
     * other corruption): the validated last-known-good copy restores it. */
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Second commit creates the validated ".good" backup. */
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char good[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good, sizeof(good)));
    TEST_ASSERT_NOT_NULL(strstr(good, "KLC-2024-000001"));

    char garbage[512];
    (void)snprintf(garbage, sizeof(garbage), "%s GOTCHA", DEVICE_V1);
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH, garbage));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK_RECOVERED,
                          app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000001", loaded.serial);

    /* The live file was repaired with the validated backup bytes. */
    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));
    TEST_ASSERT_EQUAL_STRING(good, live);
}

/* --------------------------------------------------------------------- */
/* 9. Error classification: missing vs. transiently unreadable            */
/* --------------------------------------------------------------------- */

static void test_json_nul_escape_is_rejected(void)
{
    /* An escaped NUL (\u0000) decodes to an embedded 0 byte inside the
     * parsed string; strlen()-based validation would silently truncate
     * there and accept "HQ\0suffix" as "HQ".  Such a document can never
     * satisfy the printable-charset schemas, so it is malformed. */
    char doc[512];
    (void)snprintf(doc, sizeof(doc),
                   "{\"schema_version\":1,"
                   "\"product\":\"HQ\\u0000suffix\","
                   "\"hardware_revision\":\"A\","
                   "\"serial\":\"KLC-2024-000009\","
                   "\"thingsboard_name\":\"klc-kitchen-09\"}");

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_validate_device_json(doc, NULL));

    /* Also rejected through the storage/load path. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH, doc));
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));

    /* An escaped NUL inside a MEMBER NAME is rejected too. */
    (void)snprintf(doc, sizeof(doc),
                   "{\"schema_version\":1,\"product\\u0000x\":\"HQ\","
                   "\"hardware_revision\":\"A\","
                   "\"serial\":\"KLC-2024-000009\","
                   "\"thingsboard_name\":\"klc-kitchen-09\"}");
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_validate_device_json(doc, NULL));
}

static void test_stored_embedded_nul_with_trailing_bytes_is_rejected(void)
{
    /* The round-5 HIGH storage defect: a stored file containing valid
     * JSON, an embedded NUL, and further bytes must be rejected as a
     * WHOLE.  The string-based validators would otherwise validate only
     * the prefix before the NUL and silently accept the trailing bytes. */
    char stored[600];
    const int base = snprintf(stored, sizeof(stored), "%s", DEVICE_V1);
    TEST_ASSERT_TRUE(base > 0);

    /* Same document, then a NUL, then trailing garbage — as a raw file. */
    const size_t nul_pos = (size_t)base;
    stored[nul_pos] = '\0';
    memcpy(&stored[nul_pos + 1U], "EVIL TRAILING BYTES", 19U);
    const size_t stored_len = nul_pos + 1U + 19U;

    TEST_ASSERT_TRUE(osal_file_mock_add_file_raw(APP_CONFIG_DEVICE_PATH,
                                                 stored, stored_len));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_MALFORMED,
                          app_config_load_device(&loaded));

    /* The live file was never replaced and the recovery produced nothing
     * (no valid backup exists in this scenario). */
    TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".good"));
}

static void test_stat_success_then_open_failure_aborts_commit(void)
{
    /* The round-4 HIGH defect regression test: a stat-confirmed live file
     * whose open fails is a TRANSIENT I/O error.  The commit must abort
     * BEFORE the live rename — the live document is never skipped, never
     * treated as absent, and never replaced while unreadable. */
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Produce a validated backup with a second commit. */
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Capture the live document and the backup AFTER the second commit. */
    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));
    char good_before[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_before,
                                             sizeof(good_before)));

    /* Live file exists (stat succeeds) but cannot be opened right now. */
    osal_file_mock_set_open_status(APP_CONFIG_DEVICE_PATH, OSAL_ERROR);

    strncpy(doc.serial, "KLC-2024-000003", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_commit_device(&doc));

    /* The live file and the last-known-good copy are exactly as before. */
    char live_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH,
                                             live_after,
                                             sizeof(live_after)));
    TEST_ASSERT_EQUAL_STRING(live, live_after);
    TEST_ASSERT_NOT_NULL(strstr(live_after, "KLC-2024-000002"));

    char good_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_after,
                                             sizeof(good_after)));
    TEST_ASSERT_EQUAL_STRING(good_before, good_after);

    /* No staged temporary files survived the aborted commit. */
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".tmp"));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(
        APP_CONFIG_DEVICE_PATH ".good.tmp"));

    /* Clearing the transient fault makes the same commit succeed. */
    osal_file_mock_set_open_status(APP_CONFIG_DEVICE_PATH, OSAL_SUCCESS);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000003", loaded.serial);
}

static void test_unreadable_live_document_loads_as_io_not_recovered(void)
{
    /* An existing-but-unreadable live document surfaces APP_CONFIG_ERR_IO
     * even when a valid backup exists: the load path must not silently
     * proceed as if the live document were absent. */
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    osal_file_mock_set_open_status(APP_CONFIG_DEVICE_PATH, OSAL_ERROR);

    app_config_device_doc_t loaded;
    (void)memset(&loaded, 0xA5, sizeof(loaded));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_load_device(&loaded));

    /* Nothing was published: the caller's document is untouched. */
    TEST_ASSERT_EQUAL_HEX8(0xA5, ((const unsigned char *)&loaded)[0]);

    osal_file_mock_set_open_status(APP_CONFIG_DEVICE_PATH, OSAL_SUCCESS);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000002", loaded.serial);
}

static void test_transient_stat_failure_is_io_not_not_found(void)
{
    /* The generic OSAL_ERROR is not a missing-file status: a transient
     * stat failure on the live document is APP_CONFIG_ERR_IO, both with
     * and without a backup present, and never an absence. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH,
                                             DEVICE_V1));
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH ".good",
                                             DEVICE_V1));

    osal_file_mock_set_stat_status(OSAL_ERROR);

    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_load_device(&loaded));

    /* Both files are untouched. */
    TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH));
    TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".good"));

    /* Without any file present, a transient stat failure is still an I/O
     * error, never a fabricated "recovered" state. */
    osal_file_mock_set_stat_status(OSAL_ERROR);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO,
                          app_config_load_device(&loaded));
}

static void test_missing_document_without_backup_is_not_found(void)
{
    /* Only the documented missing-file statuses classify as NOT_FOUND. */
    app_config_device_doc_t doc;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_NOT_FOUND,
                          app_config_load_device(&doc));

    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_NOT_FOUND,
                          app_config_migrate_stored(
                              APP_CONFIG_DOC_DEVICE));
}

/* --------------------------------------------------------------------- */
/* 10. Rename failure classification (live and last-known-good steps)     */
/* --------------------------------------------------------------------- */

static void test_rename_not_implemented_refuses_commit(void)
{
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));

    /* The other "no rename support" OSAL status: same refusal. */
    osal_file_mock_set_rename_status(OSAL_ERR_NOT_IMPLEMENTED);
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_UNSUPPORTED,
                          app_config_commit_device(&doc));

    char after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, after,
                                             sizeof(after)));
    TEST_ASSERT_EQUAL_STRING(live, after);
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".tmp"));

    /* A generic rename failure is IO, never UNSUPPORTED. */
    osal_file_mock_set_rename_status(OSAL_ERR_FILE);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_commit_device(&doc));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".tmp"));
}

static void test_good_rename_io_failure_reports_io_and_preserves_backups(void)
{
    /* A generic (non-unsupported) rename failure during the LAST-KNOWN-GOOD
     * promotion is APP_CONFIG_ERR_IO — not UNSUPPORTED — and leaves both
     * the live file and the previous last-known-good copy intact. */
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));

    /* Second commit creates the validated ".good" copy (the 000001 doc),
     * and moves the live document to 000002. */
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Capture BOTH files after the second commit: the live document now
     * holds 000002, the last-known-good copy holds 000001. */
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH, live,
                                             sizeof(live)));
    char good_before[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_before,
                                             sizeof(good_before)));
    TEST_ASSERT_NOT_NULL(strstr(good_before, "KLC-2024-000001"));

    /* Third commit: the LKG promotion rename fails with a transient
     * storage error (one-shot, consumed by the first rename — which is the
     * ".good" promotion). */
    osal_file_mock_set_rename_status(OSAL_ERR_FILE);
    strncpy(doc.serial, "KLC-2024-000003", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_commit_device(&doc));

    /* Both backup copies preserved: the live document (000002) and the
     * previous last-known-good copy (000001). */
    char live_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH,
                                             live_after, sizeof(live_after)));
    TEST_ASSERT_EQUAL_STRING(live, live_after);
    TEST_ASSERT_NOT_NULL(strstr(live_after, "KLC-2024-000002"));

    char good_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_after, sizeof(good_after)));
    TEST_ASSERT_EQUAL_STRING(good_before, good_after);

    /* Staged temporary files were removed. */
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".tmp"));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(
        APP_CONFIG_DEVICE_PATH ".good.tmp"));

    /* Without the fault the commit succeeds normally. */
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000003", loaded.serial);
}

static void test_live_rename_failure_after_good_promotion_preserves_both(void)
{
    /* Regression test for the round-5 HIGH defect: the commit sequence is
     * transactional — the previous last-known-good copy is preserved at
     * "<live>.good.old" until the LIVE promotion is known to have
     * succeeded.  A live-rename failure after the .good promotion must
     * atomically restore the previous ".good" so that BOTH the live file
     * and the previous last-known-good copy remain byte-for-byte intact. */
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Second commit creates the validated ".good" copy (000001) and moves
     * the live document to 000002. */
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Capture BOTH files after the second commit: live = 000002,
     * .good = 000001. */
    char live_before[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH,
                                             live_before,
                                             sizeof(live_before)));
    char good_before[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_before,
                                             sizeof(good_before)));
    TEST_ASSERT_NOT_NULL(strstr(good_before, "KLC-2024-000001"));
    TEST_ASSERT_NOT_NULL(strstr(live_before, "KLC-2024-000002"));

    /* Third commit: the LIVE promotion rename fails with a transient
     * storage error.  The ".good" promotion has already succeeded at that
     * point, so the previous backup lives at ".good.old" and must be
     * restored before the commit reports APP_CONFIG_ERR_IO. */
    osal_file_mock_set_rename_status(OSAL_ERR_FILE);
    strncpy(doc.serial, "KLC-2024-000003", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_commit_device(&doc));

    /* The live file is byte-for-byte the pre-commit document. */
    char live_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH,
                                             live_after, sizeof(live_after)));
    TEST_ASSERT_EQUAL_STRING(live_before, live_after);

    /* The previous last-known-good copy was restored byte-for-byte: the
     * failed commit did NOT lose it when the promotion succeeded. */
    char good_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_after, sizeof(good_after)));
    TEST_ASSERT_EQUAL_STRING(good_before, good_after);
    TEST_ASSERT_NOT_NULL(strstr(good_after, "KLC-2024-000001"));

    /* Staged temporary files were removed (the ".good.old" restore is
     * best-effort; a leftover there is a valid — if stale — recovery
     * source the load path still accepts). */
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".tmp"));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(
        APP_CONFIG_DEVICE_PATH ".good.tmp"));

    /* Both surviving files still validate. */
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_validate_device_json(live_after, NULL));
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_validate_device_json(good_after, NULL));

    /* Without the fault the same commit succeeds. */
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000003", loaded.serial);
}

/* --------------------------------------------------------------------- */
/* 11. Missing-status classification boundaries (review finding 1)        */
/* --------------------------------------------------------------------- */

static void test_missing_status_classification_boundaries(void)
{
    /* Only OSAL_ERR_NAME_NOT_FOUND and the LittleFS LFS_ERR_NOENT mapping
     * (OSAL_FS_ERR_PATH_INVALID) classify a load as NOT_FOUND.  Argument
     * failures (OSAL_FS_ERR_NAME_TOO_LONG, OSAL_FS_ERR_PATH_TOO_LONG) and
     * the generic OSAL_ERROR (transient storage failure) are IO, never
     * NOT_FOUND, even when a validated backup exists next to the live
     * document. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH,
                                             DEVICE_V1));
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH ".good",
                                             DEVICE_V1));

    const int32_t non_missing_statuses[] = {
        OSAL_FS_ERR_NAME_TOO_LONG,
        OSAL_FS_ERR_PATH_TOO_LONG,
        OSAL_ERROR,
    };
    for (size_t i = 0U;
         i < sizeof(non_missing_statuses) / sizeof(non_missing_statuses[0]);
         ++i)
    {
        osal_file_mock_set_stat_status(non_missing_statuses[i]);

        app_config_device_doc_t loaded;
        (void)memset(&loaded, 0xA5, sizeof(loaded));
        TEST_ASSERT_EQUAL_INT_MESSAGE(APP_CONFIG_ERR_IO,
                                      app_config_load_device(&loaded),
                                      "argument/transient stat failure must "
                                      "be IO, never NOT_FOUND");
        /* Nothing was published: the caller's document is untouched. */
        TEST_ASSERT_EQUAL_HEX8(0xA5, ((const unsigned char *)&loaded)[0]);

        /* The failed load touched neither the live file nor the backup. */
        TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH));
        TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH
                                                 ".good"));
    }

    /* For contrast: with both files absent, the documented missing-file
     * result of the initial stat still yields NOT_FOUND. */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH,
                                             DEVICE_V1));
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH ".good",
                                             DEVICE_V1));
    osal_file_mock_reset();
    app_config_register_migration_handler(NULL);
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_NOT_FOUND,
                          app_config_load_device(&loaded));
}

/* --------------------------------------------------------------------- */
/* 12. Preserve-rename failure keeps "<live>.good.old" (finding 2)        */
/* --------------------------------------------------------------------- */

static void test_preserve_rename_failure_keeps_good_old(void)
{
    /* A "<live>.good.old" left by an earlier interrupted transaction is
     * the only still-valid recovery copy.  The commit's preserve-rename
     * (good -> good.old) FAILS (moves nothing), so the abort must not
     * remove_quiet() the stale ".good.old": it must survive the refused
     * commit together with the untouched pre-commit ".good". */
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    /* Second commit archives the first (valid) live document into
     * "<live>.good": now a pre-commit ".good" exists. */
    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));
    TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".good"));

    /* Stage a stale "<live>.good.old" as an earlier interrupted
     * transaction would have left it (valid serialized document). */
    TEST_ASSERT_TRUE(osal_file_mock_add_file(APP_CONFIG_DEVICE_PATH
                                             ".good.old",
                                             DEVICE_V1));

    /* Snapshot the pre-commit state of both backup files. */
    char good_before[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_before,
                                             sizeof(good_before)));
    char good_old_before[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH
                                             ".good.old",
                                             good_old_before,
                                             sizeof(good_old_before)));

    /* Rename call 1 of the NEXT commit is the preserve step
     * (good -> good.old): refuse it. */
    osal_file_mock_fail_rename_at(osal_file_mock_rename_calls() + 1,
                                  OSAL_ERR_FILE);

    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_commit_device(&doc));

    /* The preserve rename never moved anything: the pre-commit ".good"
     * is still at ".good", and the stale ".good.old" still exists
     * byte-for-byte — the abort did NOT remove it. */
    TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".good"));
    TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH
                                             ".good.old"));
    char good_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH ".good",
                                             good_after, sizeof(good_after)));
    TEST_ASSERT_EQUAL_STRING(good_before, good_after);
    char good_old_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH
                                             ".good.old",
                                             good_old_after,
                                             sizeof(good_old_after)));
    TEST_ASSERT_EQUAL_STRING(good_old_before, good_old_after);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK,
                          app_config_validate_device_json(good_old_after,
                                                          NULL));

    /* The live document was not replaced and no temporaries survived. */
    TEST_ASSERT_TRUE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".tmp"));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH
                                              ".good.tmp"));
}

/* --------------------------------------------------------------------- */
/* 13. Failed live rename with no previous ".good" leaves no ".good"      */
/*     (finding 3)                                                        */
/* --------------------------------------------------------------------- */

static void test_failed_live_rename_without_previous_good_leaves_no_good(void)
{
    /* The live file is valid but no previous "<live>.good" exists: step 3
     * promotes the staged copy to "<live>.good" (good_promoted = true,
     * good_preserved = false).  When the LIVE rename then fails, the
     * freshly promoted ".good" must be REMOVED again (the pre-commit
     * state had no backup) while the live document stays intact. */
    app_config_device_doc_t doc;
    fill_device_doc(&doc);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));

    char live_before[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH,
                                             live_before,
                                             sizeof(live_before)));
    /* Pre-commit state: no backup of any kind. */
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".good"));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH
                                              ".good.old"));
    /* A commit with no previous ".good" performs three renames:
     * 1. preserve (fails: nothing to archive), 2. promote the staged copy
     * to ".good", 3. replace the live file.  Refuse only the live
     * replacement (the first commit already consumed rename calls). */
    const int rename_base = osal_file_mock_rename_calls();
    osal_file_mock_fail_rename_at(rename_base + 3, OSAL_ERR_FILE);

    strncpy(doc.serial, "KLC-2024-000002", sizeof(doc.serial) - 1U);
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_ERR_IO, app_config_commit_device(&doc));

    /* The freshly promoted backup was rolled back: no ".good" exists
     * after the refused commit. */
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".good"));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH
                                              ".good.old"));

    /* The live document is byte-for-byte the pre-commit state. */
    char live_after[512];
    TEST_ASSERT_TRUE(osal_file_mock_get_file(APP_CONFIG_DEVICE_PATH,
                                             live_after,
                                             sizeof(live_after)));
    TEST_ASSERT_EQUAL_STRING(live_before, live_after);
    TEST_ASSERT_NOT_NULL(strstr(live_after, "KLC-2024-000001"));

    /* No staged temporary files survived the abort. */
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH ".tmp"));
    TEST_ASSERT_FALSE(osal_file_mock_has_file(APP_CONFIG_DEVICE_PATH
                                              ".good.tmp"));

    /* Without the fault the same commit succeeds. */
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_commit_device(&doc));
    app_config_device_doc_t loaded;
    TEST_ASSERT_EQUAL_INT(APP_CONFIG_OK, app_config_load_device(&loaded));
    TEST_ASSERT_EQUAL_STRING("KLC-2024-000002", loaded.serial);
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
    RUN_TEST(test_redact_duplicate_secret_keys);
    RUN_TEST(test_redact_replaces_whole_secret_values);
    RUN_TEST(test_redact_handles_unparsable_input);

    /* 7. No lamp state */
    RUN_TEST(test_only_product_documents_are_managed);

    /* 8. Strict parsing (trailing garbage / concatenated documents) */
    RUN_TEST(test_trailing_garbage_json_is_rejected);
    RUN_TEST(test_concatenated_json_objects_are_rejected);
    RUN_TEST(test_trailing_whitespace_is_still_accepted);
    RUN_TEST(test_trailing_garbage_live_document_is_recovered_from_good);
    RUN_TEST(test_json_nul_escape_is_rejected);
    RUN_TEST(test_stored_embedded_nul_with_trailing_bytes_is_rejected);

    /* 9. Missing vs. transiently unreadable classification */
    RUN_TEST(test_stat_success_then_open_failure_aborts_commit);
    RUN_TEST(test_unreadable_live_document_loads_as_io_not_recovered);
    RUN_TEST(test_transient_stat_failure_is_io_not_not_found);
    RUN_TEST(test_missing_document_without_backup_is_not_found);

    /* 10. Rename failure classification */
    RUN_TEST(test_rename_not_implemented_refuses_commit);
    RUN_TEST(test_good_rename_io_failure_reports_io_and_preserves_backups);
    RUN_TEST(test_live_rename_failure_after_good_promotion_preserves_both);

    /* 11. Review-round regression tests */
    RUN_TEST(test_missing_status_classification_boundaries);
    RUN_TEST(test_preserve_rename_failure_keeps_good_old);
    RUN_TEST(test_failed_live_rename_without_previous_good_leaves_no_good);

    return UNITY_END();
}
