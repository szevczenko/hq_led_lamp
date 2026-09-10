/**
 * @file mqtt_cfg_tls_it_test.c
 * @brief End-to-end TLS integration tests for mqtt_cfg (TASK-110)
 *
 * Runs the production components/mqtt_cfg component against the REAL
 * Mongoose transport (mongoose.c built with MG_TLS_OPENSSL), the REAL
 * mqtt_app.c / mongoose_process.c application layers and an in-process TLS
 * MQTT broker listening on loopback.
 *
 * The broker presents the private test CA's server certificate (SAN
 * "DNS:localhost", signed by "hq-mqtt-test-ca" from platform/hq_platform/cert)
 * over TLS.  mqtt_cfg_apply() configures verified TLS through mqtt_config
 * (CA from the logical /cert path, skip-verify off, DNS hostname in the
 * address); mqtt_cfg_connect() then drives one real verified connection.
 *
 * Coverage per the TASK-110 definition of done:
 *   1. trusted CA  — mqtt.json with hostname "localhost" + the real CA in
 *      /cert/ca.crt connects through the verified handshake,
 *   2. unknown CA  — an unrelated PEM provisioned as /cert/ca.crt still
 *      configures verification (apply succeeds) and the transport handshake
 *      fails, forcing the output inactive and returning ERR_CONNECT,
 *   3. hostname mismatch — the trusted CA but a broker-hostname that does
 *      not match the server certificate SAN ("thingsboard.home.arpa"
 *      against "DNS:localhost") fails the hostname verification and forces
 *      the output inactive,
 *   4. plaintext rejection — tls_mode "mqtt" is rejected at load/apply and
 *      no transport activity can start,
 *   5. invalid path — a CA path outside /cert is rejected at load.
 *
 * In every TLS-failure case mqtt_cfg_connect() reports
 * #MQTT_CFG_ERR_CONNECT and the lamp output was forced inactive, i.e. a
 * TLS configuration/handshake failure can never enable the output.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lamp_control_mock.h"
#include "mongoose.h"
#include "mongoose_process.h"
#include "mqtt_app.h"
#include "mqtt_cfg.h"
#include "mqtt_config.h"
#include "osal_dir.h"
#include "osal_error.h"
#include "osal_file.h"
#include "osal_mount.h"
#include "osal_task.h"
#include "unity.h"

#ifndef MQTT_CFG_TEST_CERT_DIR
#define MQTT_CFG_TEST_CERT_DIR "."
#endif

/* --------------------------------------------------------------------- */
/* Test volume and broker                                                 */
/* --------------------------------------------------------------------- */

static const char *const LFS_IMAGE_PATH = "/tmp/mqtt_cfg_tls_it.img";

#define LFS_BLOCK_SIZE 4096U
#define LFS_BLOCK_COUNT 256U

/** @brief Loopback broker endpoint; the client-side hostname is a DNS name. */
#define BROKER_ADDRESS "mqtts://127.0.0.1:18884"
#define BROKER_PORT 18884u

/** @brief SAN of the broker certificate (DNS:localhost). */
#define TRUSTED_HOSTNAME "localhost"
/** @brief A valid DNS name that does NOT match the broker certificate SAN. */
#define MISMATCH_HOSTNAME "thingsboard.home.arpa"

#define CONNECT_TIMEOUT_MS 5000u

/** @brief Valid v1 document template (hostname + ca filled by the caller). */
static const char *const DOC_PREFIX =
    "{\"schema_version\":1,\"hostname\":\"%s\","
    "\"port\":%u,\"tls_mode\":\"%s\","
    "\"ca_path\":\"/cert/ca.crt\",\"client_id\":\"klc-it-%u\","
    "\"auth_mode\":\"access_token\"}";

/* --------------------------------------------------------------------- */
/* Broker (in-process TLS MQTT listener)                                  */
/* --------------------------------------------------------------------- */

static struct mg_mgr s_broker_mgr;
static bool s_broker_running;
static pthread_t s_broker_thread;

static char s_server_cert[MQTT_CERT_MAX_SIZE];
static char s_server_key[MQTT_CERT_MAX_SIZE];

/**
 * @brief Broker event handler.
 *
 * Accepted connections get the server certificate/key through mg_tls_init
 * (accepted connections do not inherit the listener's TLS context in
 * Mongoose 7.21, so the TLS setup runs on MG_EV_ACCEPT).  A client CONNECT
 * is answered with a plain CONNACK success — enough for the verified-TLS
 * connect path under test.
 */
static void broker_ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_ACCEPT)
    {
        struct mg_tls_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.cert = mg_str(s_server_cert);
        opts.key = mg_str(s_server_key);
        mg_tls_init(c, &opts);
    }
    else if (ev == MG_EV_MQTT_CMD)
    {
        const struct mg_mqtt_message *mm =
            (const struct mg_mqtt_message *)ev_data;
        if (mm->cmd == MQTT_CMD_CONNECT)
        {
            /* CONNACK, session-present=0, return-code=0 */
            static const uint8_t connack[4] = { 0x20, 0x02, 0x00, 0x00 };
            (void)mg_send(c, connack, sizeof(connack));
        }
    }
}

static void *broker_thread_run(void *arg)
{
    (void)arg;
    while (s_broker_running)
    {
        (void)mg_mgr_poll(&s_broker_mgr, 10);
    }
    return NULL;
}

static bool broker_start(void)
{
    struct mg_connection *listener = NULL;

    memset(&s_broker_mgr, 0, sizeof(s_broker_mgr));
    mg_mgr_init(&s_broker_mgr);

    listener = mg_mqtt_listen(&s_broker_mgr, BROKER_ADDRESS,
                              broker_ev_handler, NULL);
    if (listener == NULL)
    {
        return false;
    }

    s_broker_running = true;
    if (pthread_create(&s_broker_thread, NULL, broker_thread_run, NULL) != 0)
    {
        s_broker_running = false;
        return false;
    }
    return true;
}

static void broker_stop(void)
{
    if (!s_broker_running)
    {
        return;
    }
    s_broker_running = false;
    (void)pthread_join(s_broker_thread, NULL);
    mg_mgr_free(&s_broker_mgr);
}

/* --------------------------------------------------------------------- */
/* Test volume helpers                                                    */
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

static bool read_host_file(const char *host_file, char *buffer,
                           size_t buffer_size)
{
    size_t n;

    FILE *fp = fopen(host_file, "rb");
    if (fp == NULL)
    {
        return false;
    }
    n = fread(buffer, 1u, buffer_size - 1u, fp);
    (void)fclose(fp);
    if (n == 0u)
    {
        return false;
    }
    buffer[n] = '\0';
    return true;
}

static bool provision_pem(const char *host_file, const char *osal_path,
                          char *buffer, size_t buffer_size)
{
    if (!read_host_file(host_file, buffer, buffer_size))
    {
        return false;
    }
    return write_raw(osal_path, buffer, strlen(buffer));
}

static const char *host_pem_path(const char *name)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", MQTT_CFG_TEST_CERT_DIR, name);
    return path;
}

static bool write_mqtt_doc(const char *hostname, const char *tls_mode)
{
    char doc[512];
    snprintf(doc, sizeof(doc), DOC_PREFIX, hostname, (unsigned)BROKER_PORT,
             tls_mode, (unsigned)(hostname[0]));
    return write_raw(MQTT_CFG_FILE_PATH, doc, strlen(doc));
}

/** @brief (Re)provision /cert/ca.crt with @p host_pem_name. */
static bool provision_ca(const char *host_pem_name)
{
    char pem[MQTT_CERT_MAX_SIZE];
    return provision_pem(host_pem_path(host_pem_name), "/cert/ca.crt", pem,
                         sizeof(pem));
}

/* --------------------------------------------------------------------- */
/* Unity fixtures                                                         */
/* --------------------------------------------------------------------- */

void setUp(void)
{
    (void)lamp_mock_reset();

    (void)osal_unmount("/");
    (void)osal_rmfs(LFS_IMAGE_PATH);
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS,
                            osal_mkfs((char *)LFS_IMAGE_PATH, "littlefs",
                                      "config", LFS_BLOCK_SIZE,
                                      LFS_BLOCK_COUNT));
    TEST_ASSERT_EQUAL_INT32(OSAL_SUCCESS,
                            osal_mount(LFS_IMAGE_PATH, "/"));
    (void)osal_mkdir("/config");
    (void)osal_mkdir("/cert");
}

void tearDown(void)
{
    mqtt_app_deinit();
    (void)osal_unmount("/");
    (void)osal_rmfs(LFS_IMAGE_PATH);
}

/* --------------------------------------------------------------------- */
/* Tests                                                                  */
/* --------------------------------------------------------------------- */

static void test_trusted_ca_connects_verified_tls(void)
{
    bool flag = false;
    const char *str = NULL;

    TEST_ASSERT_TRUE_MESSAGE(provision_ca("ca.crt"), "provision trusted CA");
    TEST_ASSERT_TRUE_MESSAGE(write_mqtt_doc(TRUSTED_HOSTNAME, "mqtts"),
                             "write mqtt.json");

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());

    /* The applied transport must be verified TLS. */
    str = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_TRUE(mqtt_config_get_bool(&flag, MQTT_CONFIG_VALUE_SSL));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_TRUE(mqtt_config_get_bool(&flag,
                                          MQTT_CONFIG_VALUE_SKIP_VERIFY));
    TEST_ASSERT_FALSE(flag);

    /* A real verified TLS handshake over the loopback broker. */
    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_connect(CONNECT_TIMEOUT_MS));
    TEST_ASSERT_TRUE(mqtt_app_is_connected());
    TEST_ASSERT_EQUAL_UINT(0u, lamp_mock_force_inactive_calls());
}

static void test_unknown_ca_fails_handshake_and_force_off(void)
{
    /* An unrelated PEM (the broker's own server cert, not a CA) is
     * provisioned as /cert/ca.crt: verification stays enabled, the TLS
     * handshake fails at the transport and the output is forced off. */
    TEST_ASSERT_TRUE_MESSAGE(provision_ca("server.crt"), "provision bad CA");
    TEST_ASSERT_TRUE_MESSAGE(write_mqtt_doc(TRUSTED_HOSTNAME, "mqtts"),
                             "write mqtt.json");

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());
    (void)lamp_mock_reset();

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CONNECT,
                          mqtt_cfg_connect(CONNECT_TIMEOUT_MS));
    TEST_ASSERT_FALSE(mqtt_app_is_connected());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_hostname_mismatch_fails_verification_and_force_off(void)
{
    /* The trusted CA, but the broker hostname in mqtt.json does not match
     * the server certificate SAN (DNS:localhost): hostname verification
     * fails the handshake and the output is forced off. */
    TEST_ASSERT_TRUE_MESSAGE(provision_ca("ca.crt"), "provision trusted CA");
    TEST_ASSERT_TRUE_MESSAGE(write_mqtt_doc(MISMATCH_HOSTNAME, "mqtts"),
                             "write mqtt.json");

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_OK, mqtt_cfg_load_and_apply());
    (void)lamp_mock_reset();

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CONNECT,
                          mqtt_cfg_connect(CONNECT_TIMEOUT_MS));
    TEST_ASSERT_FALSE(mqtt_app_is_connected());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_plaintext_rejected_before_transport(void)
{
    /* Plaintext MQTT is rejected at load: the transport is never started
     * and the output is forced off. */
    TEST_ASSERT_TRUE_MESSAGE(provision_ca("ca.crt"), "provision trusted CA");
    TEST_ASSERT_TRUE_MESSAGE(write_mqtt_doc(TRUSTED_HOSTNAME, "mqtt"),
                             "write plaintext mqtt.json");

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_PLAINTEXT,
                          mqtt_cfg_load_and_apply());
    TEST_ASSERT_FALSE(mqtt_app_is_connected());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

static void test_invalid_ca_path_rejected(void)
{
    /* A CA path outside /cert is rejected at load; no transport touch. */
    char doc[512];
    snprintf(doc, sizeof(doc),
             "{\"schema_version\":1,\"hostname\":\"%s\","
             "\"port\":%u,\"tls_mode\":\"mqtts\","
             "\"ca_path\":\"/etc/ca.crt\",\"client_id\":\"c1\","
             "\"auth_mode\":\"access_token\"}",
             TRUSTED_HOSTNAME, (unsigned)BROKER_PORT);
    TEST_ASSERT_TRUE_MESSAGE(write_raw(MQTT_CFG_FILE_PATH, doc, strlen(doc)),
                             "write invalid-path mqtt.json");

    TEST_ASSERT_EQUAL_INT(MQTT_CFG_ERR_CA_PATH,
                          mqtt_cfg_load_and_apply());
    TEST_ASSERT_FALSE(mqtt_app_is_connected());
    TEST_ASSERT_TRUE(lamp_mock_force_inactive_calls() > 0u);
}

/* --------------------------------------------------------------------- */
/* Runner                                                                 */
/* --------------------------------------------------------------------- */

int main(void)
{
    int failures = 0;

    /* Host-side broker material: server cert/key presented by the listener.
     * Read them straight off the host filesystem — the OSAL LittleFS volume
     * is not mounted until setUp() runs per test, so nothing is written
     * through OSAL here. */
    if (!read_host_file(host_pem_path("server.crt"), s_server_cert,
                        sizeof(s_server_cert)) ||
        !read_host_file(host_pem_path("server.key"), s_server_key,
                        sizeof(s_server_key)))
    {
        printf("mqtt_cfg_tls_it: cannot read broker cert/key from %s\n",
               MQTT_CFG_TEST_CERT_DIR);
        return 2;
    }
    if (!broker_start())
    {
        printf("mqtt_cfg_tls_it: in-process TLS broker failed to start\n");
        return 2;
    }

    UNITY_BEGIN();
    MongooseProcess_Init();

    RUN_TEST(test_trusted_ca_connects_verified_tls);
    RUN_TEST(test_unknown_ca_fails_handshake_and_force_off);
    RUN_TEST(test_hostname_mismatch_fails_verification_and_force_off);
    RUN_TEST(test_plaintext_rejected_before_transport);
    RUN_TEST(test_invalid_ca_path_rejected);

    MongooseProcess_Deinit();
    failures = UNITY_END();

    broker_stop();
    return failures;
}