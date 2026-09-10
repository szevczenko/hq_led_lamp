/**
 * @file mqtt_cfg_test.c
 * @brief Host unit tests for the MQTT/TLS broker configuration owner
 *        (TASK-110)
 *
 * Runs the production components/mqtt_cfg code against:
 *   - the REAL POSIX OSAL filesystem backend on a temporary LittleFS
 *     volume (platform/hq_platform/src/osal/posix + vendored littlefs),
 *     so /config/mqtt.json and the /cert certificate files are really
 *     read through the logical OSAL paths the mount resolves,
 *   - the REAL mqtt_config codec (platform src/mongoose/mqtt_config.c),
 *     so the verified-TLS values mqtt_cfg pushes into the Mongoose
 *     transport are asserted through the actual configuration module,
 *   - a test-only mqtt_app double (mqtt_app_mock.c) and a lamp-control
 *     double (lamp_control_mock.c) for the deterministic fail-off checks.
 *
 * Coverage per the TASK-110 definition of done:
 *   1. accepted configuration always uses verified TLS — a trusted-CA
 *      document loads, applies and leaves mqtt_config with
 *      `mqtts://<dns-name>:<port>`, SSL on, skip-verify off and the
 *      logical /cert CA path in place,
 *   2. plaintext rejection — `tls_mode` other than "mqtts" (mqtt/tls/plain)
 *      is rejected,
 *   3. skip-verify rejection — `skip_verify: true` (or a wrong-typed
 *      value) is rejected,
 *   4. invalid path — empty CA path, paths outside /cert, mount-prefixed
 *      paths, traversal and a missing CA file are all rejected; the
 *      unknown-CA case configures verification to fail exactly at the
 *      transport (covered end-to-end in mqtt_cfg_tls_it_test.c),
 *   5. hostname policy — IP literals and malformed DNS names are rejected;
 *      the applied address preserves the DNS hostname (the hostname
 *      verification target),
 *   6. bounds/schema — oversized fields, out-of-range port, malformed
 *      JSON, unknown/duplicate members and unknown schema versions are
 *      rejected,
 *   7. fail-off — every load/validation/apply/connect failure forces the
 *      lamp output inactive; TLS configuration failures can never enable
 *      the output,
 *   8. mTLS mode — client cert/key are required and configured only for
 *      auth_mode "mtls", and are cleared on downgrade.
 */

#include <stdio.h>
#include <string.h>

#include "mqtt_cfg.h"
#include "mqtt_config.h"
#include "osal_dir.h"
#include "osal_error.h"
#include "osal_file.h"
#include "osal_mount.h"
#include "unity.h"

#include "lamp_control_mock.h"
#include "mqtt_app_mock.h"

#ifndef MQTT_CFG_TEST_CERT_DIR
#define MQTT_CFG_TEST_CERT_DIR "."
#endif

/* --------------------------------------------------------------------- */
/* Test volume                                                            */
/* --------------------------------------------------------------------- */

static const char *const LFS_IMAGE_PATH = "/tmp/mqtt_cfg_test.img";

#define LFS_BLOCK_SIZE 4096U
#define LFS_BLOCK_COUNT 256U

/** @brief Valid v1 document for the development endpoint. */
static const char *const DEV_MQTT_V1 =
    "{\"schema_version\":1,"
    "\"hostname\":\"thingsboard.home.arpa\","
    "\"port\":8883,"
    "\"tls_mode\":\"mqtts\","
    "\"ca_path\":\"/cert/ca.crt\","
    "\"client_id\":\"klc-kitchen-01\","
    "\"auth_mode\":\"access_token\"}";

void setUp(void)
{
    mqtt_cfg_t cfg;

    (void)lamp_mock_reset();
    (void)mqtt_app_mock_reset();

    /* Fresh LittleFS volume per test, backed by a temporary image file. */
    (void)osal_unmount("/");
    (void)osal_rmfs(LFS_IMAGE_PATH);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS,
                            osal_mkfs((char *)LFS_IMAGE_PATH, "littlefs",
                                      "config", LFS_BLOCK_SIZE,
                                      LFS_BLOCK_COUNT));
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS,
                            osal_mount(LFS_IMAGE_PATH, "/"));

    /* The boot path creates /config and /cert through the filesystem
     * bootstrap (lamp_fs); mirror that here (idempotent mkdir). */
    (void)osal_mkdir("/config");
    (void)osal_mkdir("/cert");

    /* Close the verified-apply connect gate between tests (production
     * behavior: a rejected apply clears the internal applied latch), so
     * the connect tests are deterministic regardless of run order. */
    memset(&cfg, 0, sizeof(cfg));
    (void)mqtt_cfg_apply(&cfg);
    (void)lamp_mock_reset();
}

void tearDown(void)
{
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

static bool write_doc(const char *json)
{
    return write_raw(MQTT_CFG_FILE_PATH, json, strlen(json));
}

/** @brief Copy a PEM file from the host into the LittleFS volume. */
static bool provision_pem(const char *host_file, const char *osal_path)
{
    char buf[MQTT_CFG_MAX_FILE_BYTES];
    size_t n;

    FILE *fp = fopen(host_file, "rb");
    if (fp == NULL)
    {
        return false;
    }
    n = fread(buf, 1u, sizeof(buf) - 1u, fp);
    (void)fclose(fp);
    if (n == 0u)
    {
        return false;
    }
    return write_raw(osal_path, buf, n);
}

static const char *host_pem_path(const char *name)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", MQTT_CFG_TEST_CERT_DIR, name);
    return path;
}

/* --------------------------------------------------------------------- */
/* Load + verified-transport application                                  */
/* --------------------------------------------------------------------- */

static void test_valid_trusted_ca_configures_verified_tls(void)
{
    mqtt_cfg_t cfg;
    bool flag = false;
    const char *str = NULL;
    mqtt_cert_source_t source = MQTT_CERT_SOURCE_NONE;
    const char *value = NULL;

    /* The trusted private CA is really provisioned under /cert. */
    TEST_ASSERT_TRUE_MESSAGE(
        provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"),
        "provision trusted CA");
    TEST_ASSERT_TRUE_MESSAGE(write_doc(DEV_MQTT_V1), "write mqtt.json");

    /* Load: hostname/port/client ID/TLS mode/paths are bound and parsed. */
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load(&cfg));
    TEST_ASSERT_EQUAL_UINT(MQTT_CFG_SCHEMA_VERSION, cfg.schema_version);
    TEST_ASSERT_EQUAL_STRING("thingsboard.home.arpa", cfg.hostname);
    TEST_ASSERT_EQUAL_UINT(8883u, (unsigned)cfg.port);
    TEST_ASSERT_EQUAL_STRING("klc-kitchen-01", cfg.client_id);
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_TLS_MODE_MQTTS, (int)cfg.tls_mode);
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_AUTH_ACCESS_TOKEN, (int)cfg.auth_mode);
    TEST_ASSERT_EQUAL_STRING("/cert/ca.crt", cfg.ca_path);
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());

    /* Apply: the transport must now be configured as verified TLS. */
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_apply(&cfg));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());

    str = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_EQUAL_STRING("mqtts://thingsboard.home.arpa:8883", str);
    TEST_ASSERT_EQUAL_STRING(cfg.client_id,
                             mqtt_config_get_string(MQTT_CONFIG_VALUE_CLIENT_ID));
    TEST_ASSERT_TRUE(mqtt_config_get_bool(&flag, MQTT_CONFIG_VALUE_SSL));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_TRUE(mqtt_config_get_bool(&flag,
                                          MQTT_CONFIG_VALUE_SKIP_VERIFY));
    TEST_ASSERT_FALSE(flag);
    TEST_ASSERT_TRUE(mqtt_config_get_cert_source(&source, &value,
                                                 MQTT_CONFIG_VALUE_CERT));
    TEST_ASSERT_EQUAL_INT(MQTT_CERT_SOURCE_FILE_PATH, (int)source);
    TEST_ASSERT_EQUAL_STRING("/cert/ca.crt", value);
}

static void test_load_and_apply_convenience(void)
{
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());
}

/* --------------------------------------------------------------------- */
/* Plaintext rejection                                                    */
/* --------------------------------------------------------------------- */

static void test_plaintext_tls_mode_rejected(void)
{
    mqtt_cfg_t cfg;
    const char *const modes[] = { "mqtt", "tls", "plain", "ws", "" };
    const size_t count = sizeof(modes) / sizeof(modes[0]);

    for (size_t i = 0u; i < count; ++i)
    {
        TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
        char doc[512];
        snprintf(doc, sizeof(doc),
                 "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
                 "\"port\":8883,\"tls_mode\":\"%s\","
                 "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
                 "\"auth_mode\":\"access_token\"}",
                 modes[i]);
        TEST_ASSERT_TRUE(write_doc(doc));
        (void)lamp_mock_reset();
        TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_PLAINTEXT, mqtt_cfg_load(&cfg));
        TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
    }
}

/* --------------------------------------------------------------------- */
/* skip-verify rejection                                                  */
/* --------------------------------------------------------------------- */

static void test_skip_verify_true_rejected(void)
{
    mqtt_cfg_t cfg;

    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    TEST_ASSERT_TRUE(write_raw(
        MQTT_CFG_FILE_PATH,
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
        "\"auth_mode\":\"access_token\",\"skip_verify\":true}",
        strlen("{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
               "\"port\":8883,\"tls_mode\":\"mqtts\","
               "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
               "\"auth_mode\":\"access_token\",\"skip_verify\":true}")));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_SKIP_VERIFY, mqtt_cfg_load(&cfg));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_skip_verify_wrong_type_rejected(void)
{
    mqtt_cfg_t cfg;

    TEST_ASSERT_TRUE(write_raw(
        MQTT_CFG_FILE_PATH,
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
        "\"auth_mode\":\"access_token\",\"skip_verify\":\"yes\"}",
        strlen("{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
               "\"port\":8883,\"tls_mode\":\"mqtts\","
               "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
               "\"auth_mode\":\"access_token\",\"skip_verify\":\"yes\"}")));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_MALFORMED, mqtt_cfg_load(&cfg));
}

/* --------------------------------------------------------------------- */
/* CA path policy                                                         */
/* --------------------------------------------------------------------- */

static void test_empty_ca_path_rejected(void)
{
    mqtt_cfg_t cfg;

    TEST_ASSERT_TRUE(write_raw(
        MQTT_CFG_FILE_PATH,
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"\",\"client_id\":\"c1\","
        "\"auth_mode\":\"access_token\"}",
        strlen("{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
               "\"port\":8883,\"tls_mode\":\"mqtts\","
               "\"ca_path\":\"\",\"client_id\":\"c1\","
               "\"auth_mode\":\"access_token\"}")));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CA_PATH, mqtt_cfg_load(&cfg));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_ca_path_outside_cert_rejected(void)
{
    const char *const bad_paths[] = {
        "/etc/ca.crt",                 /* outside /cert */
        "/littlefs/cert/ca.crt",       /* mount-point-prefixed (never stored) */
        "/cert",                       /* the directory itself, not a file */
        "/cert/../ca.crt",             /* traversal */
        "/certificate/ca.crt",         /* sibling directory */
        "/cert/",                      /* no filename */
    };
    const size_t count = sizeof(bad_paths) / sizeof(bad_paths[0]);

    for (size_t i = 0u; i < count; ++i)
    {
        char doc[512];
        mqtt_cfg_t cfg;

        snprintf(doc, sizeof(doc),
                 "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
                 "\"port\":8883,\"tls_mode\":\"mqtts\","
                 "\"ca_path\":\"%s\",\"client_id\":\"c1\","
                 "\"auth_mode\":\"access_token\"}",
                 bad_paths[i]);
        TEST_ASSERT_TRUE(write_doc(doc));
        (void)lamp_mock_reset();
        TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CA_PATH, mqtt_cfg_load(&cfg));
        TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
    }
}

static void test_missing_ca_file_fails_apply(void)
{
    /* Document is policy-valid (path under /cert) but the file is absent:
     * load succeeds, apply must fail — the configured transport never
     * reports success with an unresolvable CA. */
    mqtt_cfg_t cfg;

    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load(&cfg));
    (void)lamp_mock_reset();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CA_PATH, mqtt_cfg_apply(&cfg));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_invalid_ca_file_content_fails_apply(void)
{
    /* An empty / unreadable CA file under /cert must also fail apply. */
    mqtt_cfg_t cfg;

    TEST_ASSERT_TRUE(write_raw("/cert/ca.crt", "", 0u));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load(&cfg));
    (void)lamp_mock_reset();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CA_PATH, mqtt_cfg_apply(&cfg));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_unknown_ca_document_configures_verification(void)
{
    /* "Unknown CA" at the configuration layer: any existing CA file is
     * forwarded verbatim to the transport (which is where the handshake
     * fails — see mqtt_cfg_tls_it_test.c).  The component must never
     * disable verification to "help". */
    mqtt_cfg_t cfg;
    bool flag = false;
    mqtt_cert_source_t source = MQTT_CERT_SOURCE_NONE;
    const char *value = NULL;

    /* Provision a different, independently-issued CA under /cert. */
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("server.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load(&cfg));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_apply(&cfg));

    TEST_ASSERT_TRUE(mqtt_config_get_bool(&flag, MQTT_CONFIG_VALUE_SKIP_VERIFY));
    TEST_ASSERT_FALSE(flag); /* verification is never skipped */
    TEST_ASSERT_TRUE(mqtt_config_get_bool(&flag, MQTT_CONFIG_VALUE_SSL));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_TRUE(mqtt_config_get_cert_source(&source, &value,
                                                 MQTT_CONFIG_VALUE_CERT));
    TEST_ASSERT_EQUAL_INT(MQTT_CERT_SOURCE_FILE_PATH, (int)source);
    TEST_ASSERT_EQUAL_STRING("/cert/ca.crt", value);
}

/* --------------------------------------------------------------------- */
/* Hostname policy                                                        */
/* --------------------------------------------------------------------- */

static void test_ip_literal_hostname_rejected(void)
{
    const char *const ips[] = {
        "192.168.1.20", "127.0.0.1", "10.0.0.1", "0.0.0.0",
    };
    const size_t count = sizeof(ips) / sizeof(ips[0]);

    for (size_t i = 0u; i < count; ++i)
    {
        char doc[512];
        mqtt_cfg_t cfg;

        snprintf(doc, sizeof(doc),
                 "{\"schema_version\":1,\"hostname\":\"%s\","
                 "\"port\":8883,\"tls_mode\":\"mqtts\","
                 "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
                 "\"auth_mode\":\"access_token\"}",
                 ips[i]);
        TEST_ASSERT_TRUE(write_doc(doc));
        (void)lamp_mock_reset();
        TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_HOSTNAME, mqtt_cfg_load(&cfg));
        TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
    }
}

static void test_malformed_hostname_rejected(void)
{
    const char *const bad[] = {
        "",             /* empty */
        ".home.arpa",   /* leading dot */
        "home.arpa.",   /* trailing dot */
        "home..arpa",   /* empty label */
        "bad host",     /* space */
        "bad,host",     /* illegal char */
        "2001:db8::1",  /* IPv6 literal */
        "-bad",         /* dash-led label (ssh-style names allowed, but a
                           label must carry at least one alnum) */
    };
    const size_t count = sizeof(bad) / sizeof(bad[0]);

    for (size_t i = 0u; i < count; ++i)
    {
        char doc[512];
        mqtt_cfg_t cfg;

        snprintf(doc, sizeof(doc),
                 "{\"schema_version\":1,\"hostname\":\"%s\","
                 "\"port\":8883,\"tls_mode\":\"mqtts\","
                 "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
                 "\"auth_mode\":\"access_token\"}",
                 bad[i]);
        TEST_ASSERT_TRUE(write_doc(doc));
        (void)lamp_mock_reset();
        TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_HOSTNAME, mqtt_cfg_load(&cfg));
    }
}

/* --------------------------------------------------------------------- */
/* Bounds / schema                                                        */
/* --------------------------------------------------------------------- */

static void test_unknown_schema_rejected(void)
{
    mqtt_cfg_t cfg;

    TEST_ASSERT_TRUE(write_raw(
        MQTT_CFG_FILE_PATH,
        "{\"schema_version\":2,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
        "\"auth_mode\":\"access_token\"}",
        strlen("{\"schema_version\":2,\"hostname\":\"thingsboard.home.arpa\","
               "\"port\":8883,\"tls_mode\":\"mqtts\","
               "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
               "\"auth_mode\":\"access_token\"}")));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_UNKNOWN_SCHEMA, mqtt_cfg_load(&cfg));
}

static void test_malformed_json_rejected(void)
{
    const char *const bad_docs[] = {
        "not json at all",
        "{\"schema_version\":1,",                       /* truncated */
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
         "\"port\":8883,\"tls_mode\":\"mqtts\","
         "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
         "\"auth_mode\":\"access_token\",\"extra\":1}", /* unknown key */
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
         "\"port\":8883,\"tls_mode\":\"mqtts\","
         "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
         "\"auth_mode\":\"access_token\",\"schema_version\":1}", /* dup */
        "[1,2,3]",                                          /* not object */
        "{\"schema_version\":1,\"hostname\":123,"
         "\"port\":8883,\"tls_mode\":\"mqtts\","
         "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
         "\"auth_mode\":\"access_token\"}",                 /* wrong type */
    };
    const size_t count = sizeof(bad_docs) / sizeof(bad_docs[0]);

    for (size_t i = 0u; i < count; ++i)
    {
        mqtt_cfg_t cfg;

        TEST_ASSERT_TRUE(write_doc(bad_docs[i]));
        (void)lamp_mock_reset();
        TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_MALFORMED, mqtt_cfg_load(&cfg));
    }
}

static void test_field_bounds_rejected(void)
{
    mqtt_cfg_t cfg;

    /* hostname beyond the 128-char bound */
    char long_host[MQTT_CFG_HOSTNAME_MAX_LEN + 8u];
    memset(long_host, 'a', sizeof(long_host) - 1u);
    long_host[sizeof(long_host) - 1u] = '\0';
    memcpy(long_host, "a.", 2u); /* keep it DNS-shaped */

    char doc[512];
    snprintf(doc, sizeof(doc),
             "{\"schema_version\":1,\"hostname\":\"%s\","
             "\"port\":8883,\"tls_mode\":\"mqtts\","
             "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
             "\"auth_mode\":\"access_token\"}",
             long_host);
    TEST_ASSERT_TRUE(write_doc(doc));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_BOUNDS, mqtt_cfg_load(&cfg));

    /* port outside the valid range */
    TEST_ASSERT_TRUE(write_raw(
        MQTT_CFG_FILE_PATH,
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":70000,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
        "\"auth_mode\":\"access_token\"}",
        strlen("{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
               "\"port\":70000,\"tls_mode\":\"mqtts\","
               "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
               "\"auth_mode\":\"access_token\"}")));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_BOUNDS, mqtt_cfg_load(&cfg));

    /* empty client ID */
    TEST_ASSERT_TRUE(write_raw(
        MQTT_CFG_FILE_PATH,
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"\","
        "\"auth_mode\":\"access_token\"}",
        strlen("{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
               "\"port\":8883,\"tls_mode\":\"mqtts\","
               "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"\","
               "\"auth_mode\":\"access_token\"}")));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_BOUNDS, mqtt_cfg_load(&cfg));
}

static void test_missing_document_forces_fail_off(void)
{
    mqtt_cfg_t cfg;

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_NOT_FOUND, mqtt_cfg_load(&cfg));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

/* --------------------------------------------------------------------- */
/* mTLS mode                                                              */
/* --------------------------------------------------------------------- */

static void test_mtls_requires_client_paths(void)
{
    mqtt_cfg_t cfg;
    const char *const docs[] = {
        /* mtls without any client material */
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
        "\"auth_mode\":\"mtls\"}",
        /* mtls with only the certificate */
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
        "\"client_cert_path\":\"/cert/device.crt\","
        "\"auth_mode\":\"mtls\"}",
        /* cert path outside /cert */
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
        "\"client_cert_path\":\"/tmp/device.crt\","
        "\"client_key_path\":\"/cert/device.key\","
        "\"auth_mode\":\"mtls\"}",
    };
    const size_t count = sizeof(docs) / sizeof(docs[0]);

    for (size_t i = 0u; i < count; ++i)
    {
        TEST_ASSERT_TRUE(write_doc(docs[i]));
        (void)lamp_mock_reset();
        TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CERT_PATH, mqtt_cfg_load(&cfg));
    }
}

static void test_mtls_apply_configures_and_downgrade_clears(void)
{
    const char *mtls_doc =
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtts\","
        "\"ca_path\":\"/cert/ca.crt\","
        "\"client_cert_path\":\"/cert/device.crt\","
        "\"client_key_path\":\"/cert/device.key\","
        "\"client_id\":\"c1\",\"auth_mode\":\"mtls\"}";
    mqtt_cfg_t cfg;
    mqtt_cert_source_t source = MQTT_CERT_SOURCE_NONE;
    const char *value = NULL;

    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("client.crt"),
                                   "/cert/device.crt"));
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("client.key"),
                                   "/cert/device.key"));
    TEST_ASSERT_TRUE(write_doc(mtls_doc));

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load(&cfg));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_AUTH_MTLS, (int)cfg.auth_mode);
    TEST_ASSERT_EQUAL_STRING("/cert/device.crt", cfg.client_cert_path);
    TEST_ASSERT_EQUAL_STRING("/cert/device.key", cfg.client_key_path);
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_apply(&cfg));

    TEST_ASSERT_TRUE(mqtt_config_get_cert_source(
        &source, &value, MQTT_CONFIG_VALUE_CLIENT_CERT));
    TEST_ASSERT_EQUAL_INT(MQTT_CERT_SOURCE_FILE_PATH, (int)source);
    TEST_ASSERT_EQUAL_STRING("/cert/device.crt", value);
    TEST_ASSERT_TRUE(mqtt_config_get_cert_source(
        &source, &value, MQTT_CONFIG_VALUE_CLIENT_KEY));
    TEST_ASSERT_EQUAL_INT(MQTT_CERT_SOURCE_FILE_PATH, (int)source);
    TEST_ASSERT_EQUAL_STRING("/cert/device.key", value);

    /* Downgrade to access_token: mTLS material must be cleared so a
     * previously applied client certificate cannot linger. */
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load(&cfg));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_apply(&cfg));
    TEST_ASSERT_TRUE(mqtt_config_get_cert_source(
        &source, &value, MQTT_CONFIG_VALUE_CLIENT_CERT));
    TEST_ASSERT_EQUAL_INT(MQTT_CERT_SOURCE_NONE, (int)source);
    TEST_ASSERT_TRUE(mqtt_config_get_cert_source(
        &source, &value, MQTT_CONFIG_VALUE_CLIENT_KEY));
    TEST_ASSERT_EQUAL_INT(MQTT_CERT_SOURCE_NONE, (int)source);
}

/* --------------------------------------------------------------------- */
/* Verified TLS connect + fail-off                                        */
/* --------------------------------------------------------------------- */

static void test_connect_success_no_fail_off(void)
{
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());

    /* The transport connects; the component reports success. */
    mqtt_app_mock_simulate_connect();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_connect(0u));
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());
    TEST_ASSERT_TRUE(mqtt_app_mock_init_calls() > 0u);
}

static void test_connect_failure_forces_fail_off(void)
{
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());
    (void)lamp_mock_reset();

    /* A TLS/transport failure (unknown CA, hostname mismatch, handshake
     * error) must drive the fail-off and report an error. */
    mqtt_app_mock_simulate_connect_failure(
        MQTT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR);
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CONNECT, mqtt_cfg_connect(0u));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_connect_timeout_forces_fail_off(void)
{
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());
    (void)lamp_mock_reset();

    /* The broker never answers: the bounded wait expires with the output
     * forced off and ThingsBoard stays blocked. */
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CONNECT,
                          mqtt_cfg_connect(60u));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_connect_without_verified_apply_rejected(void)
{
    /* No verified configuration was applied (SSL is off): connect must be
     * refused before any transport activity, forcing the output off. */
    TEST_ASSERT_TRUE(mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SSL));
    (void)lamp_mock_reset();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_NOT_APPLIED, mqtt_cfg_connect(0u));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_connect_refuses_manufactured_ssl_state(void)
{
    /* An external setter must not be able to manufacture an SSL-enabled
     * transport without a successful validated apply: the transport never
     * starts and the barrier is not released. */
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(mqtt_config_set_string(
        "mqtts://thingsboard.home.arpa:8883", MQTT_CONFIG_VALUE_ADDRESS));
    TEST_ASSERT_TRUE(mqtt_config_set_string(
        "klc-kitchen-01", MQTT_CONFIG_VALUE_CLIENT_ID));
    TEST_ASSERT_TRUE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL));
    TEST_ASSERT_TRUE(mqtt_config_set_bool(false,
                                          MQTT_CONFIG_VALUE_SKIP_VERIFY));
    TEST_ASSERT_TRUE(mqtt_config_set_cert_source(
        MQTT_CERT_SOURCE_FILE_PATH, "/cert/ca.crt", MQTT_CONFIG_VALUE_CERT));
    (void)lamp_mock_reset();

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_NOT_APPLIED, mqtt_cfg_connect(0u));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_release_calls());
    TEST_ASSERT_EQUAL_UINT(0u, mqtt_app_mock_init_calls());
}

static void test_failed_apply_keeps_fail_off_latched(void)
{
    /* A rejected apply (missing/unreadable CA file) closes the connect
     * gate: a later connect is refused, the transport never starts and the
     * fail-off barrier stays latched (release must not run). */
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    (void)lamp_mock_reset();

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CA_PATH, mqtt_cfg_load_and_apply());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);

    (void)lamp_mock_reset();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_NOT_APPLIED, mqtt_cfg_connect(0u));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_release_calls());
    TEST_ASSERT_EQUAL_UINT(0u, mqtt_app_mock_init_calls());
}

static void test_connect_success_releases_fail_off(void)
{
    /* The fail-off barrier is released only by a successful verified TLS
     * connect (valid configuration applied AND verified connection
     * established); a failed connect keeps it latched. */
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());
    (void)lamp_mock_reset();

    mqtt_app_mock_simulate_connect();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_connect(0u));
    TEST_ASSERT_TRUE(lamp_mock_release_calls() > 0u);
    TEST_ASSERT_TRUE(mqtt_app_mock_init_calls() > 0u);
}

static void test_connect_failure_does_not_release_fail_off(void)
{
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());
    (void)lamp_mock_reset();

    /* A TLS/transport failure forces the output off and must NOT release
     * the barrier. */
    mqtt_app_mock_simulate_connect_failure(
        MQTT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR);
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CONNECT, mqtt_cfg_connect(0u));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_release_calls());
}

static void test_disconnect_after_verified_connect_forces_fail_off(void)
{
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());

    mqtt_app_mock_simulate_connect();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_connect(0u));
    (void)lamp_mock_reset();

    /* A post-connect remote/TLS loss must turn the output off immediately;
     * automatic reconnect is not allowed to release the barrier. */
    mqtt_app_mock_simulate_disconnect(MQTT_DISCONNECT_REASON_REMOTE_CLOSE);
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_release_calls());
}

static void test_mutated_transport_state_is_rejected(void)
{
    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());

    /* The public mqtt_config API remains mutable, but a post-apply mutation
     * must not bypass the accepted DNS endpoint snapshot. */
    TEST_ASSERT_TRUE(mqtt_config_set_string(
        "mqtts://192.0.2.10:8883", MQTT_CONFIG_VALUE_ADDRESS));
    (void)lamp_mock_reset();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_NOT_APPLIED, mqtt_cfg_connect(0u));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
    TEST_ASSERT_EQUAL_UINT(0u, mqtt_app_mock_init_calls());
}

static void test_load_failure_leaves_output_object_untouched(void)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_t before;

    memset(&cfg, 0xA5, sizeof(cfg));
    before = cfg;
    TEST_ASSERT_TRUE(write_raw(
        MQTT_CFG_FILE_PATH,
        "{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
        "\"port\":8883,\"tls_mode\":\"mqtt\","
        "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
        "\"auth_mode\":\"access_token\"}",
        strlen("{\"schema_version\":1,\"hostname\":\"thingsboard.home.arpa\","
               "\"port\":8883,\"tls_mode\":\"mqtt\","
               "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"c1\","
               "\"auth_mode\":\"access_token\"}")));

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_PLAINTEXT, mqtt_cfg_load(&cfg));
    TEST_ASSERT_EQUAL_MEMORY(&before, &cfg, sizeof(cfg));
}

static void test_load_null_argument_forces_fail_off(void)
{
    /* Every load failure — including an invalid argument — forces the
     * output inactive (fail-off contract). */
    (void)lamp_mock_reset();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_INVALID_ARGUMENT,
                          mqtt_cfg_load(NULL));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_invalid_auth_mode_rejected(void)
{
    /* A hand-built configuration with an unknown authentication enum must
     * be rejected (malformed), never silently treated as non-mTLS. */
    mqtt_cfg_t cfg;

    TEST_ASSERT_TRUE(provision_pem(host_pem_path("ca.crt"), "/cert/ca.crt"));
    TEST_ASSERT_TRUE(write_doc(DEV_MQTT_V1));
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load(&cfg));

    cfg.auth_mode = (mqtt_cfg_auth_mode_t)99;
    (void)lamp_mock_reset();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_MALFORMED, mqtt_cfg_validate(&cfg));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);

    (void)lamp_mock_reset();
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_MALFORMED, mqtt_cfg_apply(&cfg));
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

/* --------------------------------------------------------------------- */
/* Runner                                                                 */
/* --------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_valid_trusted_ca_configures_verified_tls);
    RUN_TEST(test_load_and_apply_convenience);
    RUN_TEST(test_plaintext_tls_mode_rejected);
    RUN_TEST(test_skip_verify_true_rejected);
    RUN_TEST(test_skip_verify_wrong_type_rejected);
    RUN_TEST(test_empty_ca_path_rejected);
    RUN_TEST(test_ca_path_outside_cert_rejected);
    RUN_TEST(test_missing_ca_file_fails_apply);
    RUN_TEST(test_invalid_ca_file_content_fails_apply);
    RUN_TEST(test_unknown_ca_document_configures_verification);
    RUN_TEST(test_ip_literal_hostname_rejected);
    RUN_TEST(test_malformed_hostname_rejected);
    RUN_TEST(test_unknown_schema_rejected);
    RUN_TEST(test_malformed_json_rejected);
    RUN_TEST(test_field_bounds_rejected);
    RUN_TEST(test_missing_document_forces_fail_off);
    RUN_TEST(test_mtls_requires_client_paths);
    RUN_TEST(test_mtls_apply_configures_and_downgrade_clears);
    RUN_TEST(test_connect_success_no_fail_off);
    RUN_TEST(test_connect_failure_forces_fail_off);
    RUN_TEST(test_connect_timeout_forces_fail_off);
    RUN_TEST(test_connect_without_verified_apply_rejected);
    RUN_TEST(test_connect_refuses_manufactured_ssl_state);
    RUN_TEST(test_failed_apply_keeps_fail_off_latched);
    RUN_TEST(test_connect_success_releases_fail_off);
    RUN_TEST(test_connect_failure_does_not_release_fail_off);
    RUN_TEST(test_disconnect_after_verified_connect_forces_fail_off);
    RUN_TEST(test_mutated_transport_state_is_rejected);
    RUN_TEST(test_load_failure_leaves_output_object_untouched);
    RUN_TEST(test_load_null_argument_forces_fail_off);
    RUN_TEST(test_invalid_auth_mode_rejected);

    return UNITY_END();
}