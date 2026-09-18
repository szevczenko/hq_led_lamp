/**
 * @file app_main.c
 * @brief Kitchen LED Controller application entry point.
 *
 *  ## Boot-order contract (TASK-107 / TASK-118 / TASK-109 / TASK-115 /
 *                        TASK-127)
 *
 *   1. lamp_control_init() — the output is initialized off and stays off
 *      until a valid desired state arrives.
 *   2. The application state machine (app_state, TASK-115) is started
 *      next: it is the explicit owner of the sequence
 *      boot -> safe-off -> filesystem -> configuration -> Wi-Fi ->
 *      provisioning (only when no saved station credential exists) ->
 *      verified MQTT/TLS -> state sync -> online, of every legal
 *      transition (with an explicit transition owner per event), of the
 *      stale-callback (generation/session) rejection, of the bounded
 *      retry/backoff schedule and of the application watchdog.
 *   3. lamp_fs_init() — mounts the LittleFS "storage" partition at
 *      /littlefs through the OSAL and creates /cert, /config, /state
 *      idempotently.  The mount never formats (TASK-118 removed the
 *      backend's format_if_mount_failed), and lamp_fs never calls
 *      osal_mkfs()/osal_rmfs() on the boot path: formatting is reserved
 *      for explicit manufacturing/provisioning operations.
 *   4. On any filesystem failure the state machine moves to SAFE_OFF
 *      (degraded) and the fail-off invariant forces the output inactive;
 *      storage is preserved untouched and configuration is not loaded.
 *      A failed directory bootstrap unmounts the volume before returning.
 *   5. Only a fully successful bootstrap reaches configuration loading.
 *      Configuration loading itself is ordered: product documents
 *      (device.json / manufacturing.json, TASK-108) and the broker/TLS
 *      document (mqtt.json, TASK-110) first, then the device identity
 *      (TASK-111).  A rejected configuration parks the machine degraded
 *      (SAFE_OFF) with no automatic retry — recovery is an explicit
 *      provisioning/reset/OTA action.
 *   6. Device identity (TASK-111) is loaded after configuration validation
 *      and before any ThingsBoard initialization.  device_identity_load()
 *      only trusts validated manufacturing/configuration records, rejects
 *      missing, empty, oversized or malformed identity input, never logs
 *      the token, and is FATAL safe-off: a missing or invalid identity
 *      forces the output inactive and blocks the ThingsBoard session.
 *   7. Wi-Fi onboarding (TASK-109) runs next through the product-owned
 *      network adapter.  The machine's NETWORK gate is the consumed
 *      network_manager_is_connected() polling loop below (the successor
 *      of network_manager_wait_connected()): ThingsBoard must never
 *      connect before this gate passes.  A Wi-Fi loss forces the adapter
 *      to fail the lamp off synchronously; the supervisor converts the
 *      disconnect into the machine's DISCONNECTED event within one
 *      supervisor cadence.
 *   7b. Wi-Fi provisioning (TASK-127) sits between the Wi-Fi gate and TLS,
 *      entered ONLY when the adapter reports no saved station credential
 *      (wifi_provisioning_manager_has_saved_credentials()): the machine
 *      enters PROVISIONING on PROVISIONING_STARTED, the supervisor starts
 *      the provisioning portal (HTTP portal + captive DNS on the shared
 *      Mongoose process) and waits for the station to connect while
 *      feeding the watchdog in-loop.  On success the portal stays up for
 *      a bounded success-grace interval
 *      (CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS — the single grace
 *      source of truth since TASK-131; the duplicate KLC knob was dropped),
 *      is stopped through the adapter (listeners closed; the shared Mongoose
 *      process and MQTT/TLS untouched), and PROVISIONING_SUCCEEDED hands
 *      control back to the NETWORK gate, which then passes the now
 *      credentialed station to TLS.  A credentialed device never enters
 *      provisioning — it passes the NETWORK gate straight to TLS.
 *      ThingsBoard STILL never starts before a verified network
 *      connection: provisioning only ever hands control back to the
 *      NETWORK gate, never past it.
 *   7c. Platform fallback controller (TASK-131): the automatic fallback
 *      controller (wifi_provisioning_controller.c) is COMPILED IN
 *      (CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK=y, bounded
 *      FALLBACK_ATTEMPTS budget) but NOT yet wired — app_main does not
 *      register its notification hook or start it (wiring is TASK-132/133).
 *      While half-wired the controller is inert and the product decision in
 *      7b remains the only provisioning driver; the intermediate,
 *      half-wired state is deliberate and explicit.
 *   8. The single re-enable transition for the lamp fail-off barrier is a
 *      successful verified MQTT/TLS connection (mqtt_cfg_connect(),
 *      TASK-110), consumed at the TLS gate.  A Wi-Fi connection alone, an
 *      invalid or missing identity, or any rejected broker/TLS
 *      configuration can never enable the output.
 *
 *  ## Supervisor loop and watchdog ownership (TASK-115)
 *
 *    app_main() never returns to an idle state: it runs a supervisor loop
 *    that is the SINGLE watchdog-feeding task.  All machine events that
 *    this task produces (filesystem result, configuration result, network
 *    gate result, provisioning result, TLS gate result) are delivered from
 *    this task; the Wi-Fi adapter's callbacks remain informational (the
 *    adapter's own synchronous fail-off covers the safety latency, and the
 *    supervisor picks the disconnect up within one cadence).  Blocking
 *    constraints honored here:
 *      - every blocking call in the loop is bounded BELOW the configured
 *        application watchdog window (60 s): the network gate polls every
 *        NETWORK_WAIT_POLL_INTERVAL_MS (50 ms) and calls app_state_poll()
 *        in each iteration (same task — the single owner),
 *      - the provisioning gate waits in the same in-loop, feed-every-
 *        iteration pattern (no single blocking call spans the watchdog
 *        window), and never blocks from a Mongoose/Wi-Fi callback context
 *        (all of it runs in the supervisor task),
 *      - the verified-TLS connect is bounded by mqtt_cfg_connect()'s own
 *        30 s window, which is below the 60 s watchdog,
 *      - app_state_poll() is never called from a worker callback context.
 *    Recovery: the watchdog expiry ends in the machine's FATAL state and
 *    leaves the app task; an explicit external reset or OTA re-arms the
 *    sequence at boot.
 *
 *  The idempotent mkdir (EEXIST -> OSAL_ERR_NAME_TAKEN -> LAMP_FS_OK) is
 *  expected after a reflash onto persistent storage and is not a defect.
 */

#include <stdbool.h>

#include "esp_log.h"

#include "app_config.h"
#include "app_state.h"
#include "device_identity.h"
#include "hal_types.h"
#include "lamp_control.h"
#include "lamp_fs.h"
#include "mongoose_process.h"
#include "mqtt_cfg.h"
#include "network_manager.h"
#include "osal_task.h"
#include "sdkconfig.h"
#include "wifi_provisioning_manager.h"

static const char *TAG = "klc";

/** @brief Supervisor cadence: the watchdog feed interval. */
#define APP_SUPERVISE_PERIOD_MS 50u

/**
 * @brief Application watchdog window (ms).
 *
 * Larger than the longest single blocking call in the supervisor loop:
 * mqtt_cfg_connect() is bounded by NETWORK_CONNECT_TIMEOUT_MS (30 s) and
 * the network gate waits in <=50 ms poll iterations while feeding, so a
 * 60 s window keeps a healthy boot from ever tripping the watchdog while
 * still detecting a real stall (see the blocking constraints in
 * app_state.h).
 */
#define APP_SUPERVISE_WATCHDOG_MS 60000u

/**
 * @brief Bounded window [ms] the PROVISIONING gate waits for the station to
 *        connect while the provisioning portal is up.
 *
 * Bounded so a provisioning failure always parks the machine degraded with
 * the existing bounded backoff (never a restart storm).  The wait feeds the
 * watchdog in-loop, so this window never trips the 60 s watchdog; it bounds
 * only the user's window to submit a credential at the portal.
 */
#define PROVISIONING_WAIT_TIMEOUT_MS 180000u

/* --------------------------------------------------------------------- */
/* Network adapter wiring (TASK-109)                                       */
/* --------------------------------------------------------------------- */

/**
 * @brief Network connection established (Wi-Fi manager worker context).
 *
 * Informational: the adapter's connection state is consumed by the
 * supervisor's NETWORK gate (the machine's single network gate — see the
 * file header).  Nothing here touches Wi-Fi credentials — the adapter and
 * the manager keep them private.  Fail-off re-enable is owned by
 * mqtt_cfg_connect() (TASK-110), never by this callback.
 */
static void on_network_connected(void *context)
{
    (void)context;
    ESP_LOGI(TAG, "Network connected; verified MQTT/TLS connect is now "
                  "allowed (fail-off release is owned by mqtt_cfg_connect)");
}

/**
 * @brief Network lost or connect attempts exhausted (Wi-Fi worker context).
 *
 * The adapter has already forced the lamp output inactive synchronously on
 * this path (fail-off within application latency); the supervisor picks the
 * disconnect up through the machine's DISCONNECTED event within one
 * supervisor cadence (the machine transition itself also fails the output
 * off — idempotent).
 */
static void on_network_disconnected(void *context)
{
    (void)context;
    ESP_LOGW(TAG, "Network lost; lamp forced off by adapter, "
                  "ThingsBoard stays disconnected until reconnect");
}

/**
 * @brief Bring up the Wi-Fi manager (station mode) behind the TASK-109
 *        gate.  Returns true when the network is started.
 */
static bool start_network(void)
{
    static const network_callbacks_t callbacks = {
        .on_connected    = on_network_connected,
        .on_disconnected = on_network_disconnected,
        .context         = NULL,
    };

    const int status = network_manager_start(&callbacks);
    if (status != NETWORK_OK)
    {
        ESP_LOGE(TAG, "network_manager_start failed: %d (offline, output off)",
                 (int)status);
        return false;
    }

    return true;
}

/**
 * @brief The machine's NETWORK gate: a bounded wait for Wi-Fi with in-loop
 *        watchdog feeding (blocking constraint: one task owns the feed and
 *        polls inside every bounded wait).
 *
 * This is the consumed successor of network_manager_wait_connected(): it
 * polls network_manager_is_connected() at NETWORK_WAIT_POLL_INTERVAL_MS
 * and calls app_state_poll() in each iteration, so the watchdog is fed by
 * the same (only) owner task while the gate is pending.
 *
 * @return true on a connection, false on timeout (the caller delivers
 *         NETWORK_CONNECTED / NETWORK_FAILED accordingly).
 */
static bool supervise_network_gate(void)
{
    uint32_t waited_ms = 0U;

    while (!network_manager_is_connected() &&
           (waited_ms < NETWORK_CONNECT_TIMEOUT_MS))
    {
        app_state_poll(); /* feed — this task is the single watchdog owner */
        (void)osal_task_delay_ms(NETWORK_WAIT_POLL_INTERVAL_MS);
        waited_ms += NETWORK_WAIT_POLL_INTERVAL_MS;
    }

    if (!network_manager_is_connected())
    {
        ESP_LOGW(TAG, "No Wi-Fi connection within %u ms; "
                      "verified TLS connect stays blocked",
                 (unsigned)NETWORK_CONNECT_TIMEOUT_MS);
    }
    return network_manager_is_connected();
}

/* --------------------------------------------------------------------- */
/* Provisioning portal (TASK-127)                                          */
/* --------------------------------------------------------------------- */

/**
 * @brief Bounded wait for the station to connect while the provisioning
 *        portal is up.
 *
 * The user drives the captive portal (HTTP portal + captive DNS on the
 * shared Mongoose process) to submit a credential; once the station connects
 * the portal's job is done.  This bounded wait polls
 * network_manager_is_connected() at NETWORK_WAIT_POLL_INTERVAL_MS and feeds
 * the watchdog through app_state_poll() in every iteration — the same
 * single-owner feed pattern as the NETWORK gate, so no blocking call ever
 * spans the watchdog window.
 *
 * @return true when the station connected; false when the provisioning wait
 *         window elapsed without a connection.
 */
static bool wait_station_connected_while_provisioning(void)
{
    uint32_t waited_ms = 0U;

    while (!network_manager_is_connected() &&
           (waited_ms < PROVISIONING_WAIT_TIMEOUT_MS))
    {
        app_state_poll(); /* feed — this task is the single watchdog owner */
        (void)osal_task_delay_ms(NETWORK_WAIT_POLL_INTERVAL_MS);
        waited_ms += NETWORK_WAIT_POLL_INTERVAL_MS;
    }

    return network_manager_is_connected();
}

/**
 * @brief Keep the provisioning portal up for the bounded success-grace
 *        interval after the station connects, feeding the watchdog in-loop.
 *
 * Mirrors the platform controller's grace/retire pattern: keep the AP (and
 * its HTTP/DNS listeners) available briefly so the freshly provisioned
 * station settles, then stop the portal.  The interval is bounded and
 * Kconfig-configurable (CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS —
 * the platform knob is the single grace source of truth since TASK-131; the
 * product duplicate CONFIG_KLC_PROVISIONING_GRACE_MS was dropped); a value
 * of 0 retires immediately.  Every iteration feeds the watchdog so the
 * grace wait never spans the watchdog window.
 */
static void honor_provisioning_success_grace(void)
{
    uint32_t grace_ms = (uint32_t)CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS;
    uint32_t waited_ms = 0U;

    while (waited_ms < grace_ms)
    {
        app_state_poll(); /* feed — this task is the single watchdog owner */
        (void)osal_task_delay_ms(NETWORK_WAIT_POLL_INTERVAL_MS);
        waited_ms += NETWORK_WAIT_POLL_INTERVAL_MS;
    }
}

/**
 * @brief Stop the provisioning portal through the adapter if it is active.
 *
 * Closes only the portal's HTTP/DNS listeners; the shared Mongoose process
 * (and with it MQTT/TLS) is never torn down.  Safe to call at any time —
 * stop() is idempotent and a no-op when the portal is already stopped.  Used
 * to guarantee the portal never strands a listener on success, failure, OTA
 * entry or FATAL.
 */
static void stop_provisioning_if_active(void)
{
    if (wifi_provisioning_manager_is_active())
    {
        (void)wifi_provisioning_manager_stop();
    }
}

/**
 * @brief The machine's PROVISIONING gate: run the portal until the station
 *        connects (bounded), honor the success-grace interval, then stop the
 *        portal.
 *
 * Called only in the machine's PROVISIONING state (entered from NETWORK on
 * PROVISIONING_STARTED when the adapter reported no saved station
 * credential).  All blocking work runs in the supervisor task and feeds the
 * watchdog in-loop.  The portal is ALWAYS stopped before returning: on
 * success after the grace interval, on failure immediately.  A start failure
 * or a wait-window expiry returns false and the supervisor delivers
 * PROVISIONING_FAILED, parking the machine in SAFE_OFF with the existing
 * bounded retry — never a restart storm.
 *
 * @return true when the station connected and the portal was retired;
 *         false when startup failed or the wait window expired.
 */
static bool supervise_provisioning(void)
{
    bool connected;

    /* Start the portal (HTTP + captive DNS on the shared Mongoose process).
     * A clean refusal here (e.g. the shared process is not running) is a
     * provisioning failure that parks degraded — we never retry the portal
     * from within this gate. */
    if (wifi_provisioning_manager_start() != WIFI_PROVISIONING_MANAGER_OK)
    {
        ESP_LOGE(TAG, "Provisioning portal start failed; "
                      "machine parks degraded (bounded retry)");
        return false;
    }

    ESP_LOGI(TAG, "Provisioning portal running; waiting for the station to "
                  "submit a credential and connect (bounded wait, "
                  "watchdog fed)");

    connected = wait_station_connected_while_provisioning();
    if (!connected)
    {
        ESP_LOGW(TAG, "No station connection within %u ms; stopping the "
                      "portal and parking degraded",
                 (unsigned)PROVISIONING_WAIT_TIMEOUT_MS);
        stop_provisioning_if_active();
        return false;
    }

    /* Station connected while the portal was up: keep the AP up briefly for
     * the success-grace interval, then retire the portal cleanly (listeners
     * closed; the shared Mongoose process and MQTT/TLS untouched). */
    honor_provisioning_success_grace();
    stop_provisioning_if_active();
    ESP_LOGI(TAG, "Provisioning succeeded and portal retired; "
                  "continuing to the NETWORK gate -> TLS");
    return true;
}

/* --------------------------------------------------------------------- */
/* Lamp output                                                            */
/* --------------------------------------------------------------------- */

static bool init_lamp_output(void)
{
    /* CONFIG_KLC_* values come from the IDF Kconfig (sdkconfig.h).  A bool
     * option that is "not set" is simply not #defined, so the polarity is
     * selected in preprocessor context (undefined -> 0 in #if). */
    lamp_control_config_t config = {
        .pin          = (hal_pin_t)CONFIG_KLC_LED_PWM_GPIO,
        .frequency_hz = (uint32_t)CONFIG_KLC_LED_PWM_FREQUENCY_HZ,
#if CONFIG_KLC_LED_PWM_ACTIVE_LOW
        .polarity     = HAL_POLARITY_ACTIVE_LOW,
#else
        .polarity     = HAL_POLARITY_ACTIVE_HIGH,
#endif
    };

    lamp_status_t status = lamp_control_init(&config);
    if (status != LAMP_OK)
    {
        ESP_LOGE(TAG, "lamp_control_init failed: %d (output off)", (int)status);
        return false;
    }

    /* The lamp is initialized with its output off; it stays off until a
     * valid desired state arrives. */
    return true;
}

/* --------------------------------------------------------------------- */
/* Filesystem bootstrap                                                   */
/* --------------------------------------------------------------------- */

/**
 * @brief Fail-safe invoked on any filesystem bootstrap failure: forces the
 *        lamp output to the logical INACTIVE level.  The state machine's
 *        non-online transition to SAFE_OFF performs the same fail-off
 *        (idempotent); this callback keeps the guarantee for the bootstrap
 *        path itself.
 */
static void lamp_fs_fail_safe(void)
{
    lamp_status_t status = lamp_control_force_inactive();
    if (status != LAMP_OK)
    {
        ESP_LOGE(TAG, "Filesystem fail-safe could not force output off: %d",
                 (int)status);
    }
}

/**
 * @return true when the filesystem is mounted and the directory layout is
 *         ready; false when boot must degrade (output off, no config load).
 */
static bool bootstrap_filesystem(void)
{
    lamp_fs_config_t fs_config = {
        .fail_safe_cb = lamp_fs_fail_safe,
    };

    lamp_fs_status_t status = lamp_fs_init(&fs_config);
    if (status != LAMP_FS_OK)
    {
        ESP_LOGE(TAG, "Filesystem bootstrap failed: %d "
                      "(storage preserved, output off, configuration not loaded)",
                 (int)status);
        return false;
    }

    ESP_LOGI(TAG, "Filesystem ready: %s mounted at %s (cert/, config/, state/)",
             LAMP_FS_PARTITION_LABEL, LAMP_FS_MOUNT_POINT);
    return true;
}

/* --------------------------------------------------------------------- */
/* Configuration loading (TASK-108)                                        */
/* --------------------------------------------------------------------- */

/**
 * @brief Load the product documents through the configuration service.
 *
 * @return true when both documents are usable (loaded or recovered).
 */
static bool load_product_configuration(void)
{
    app_config_device_doc_t device;
    app_config_status_t status = app_config_load_device(&device);
    if ((status != APP_CONFIG_OK) && (status != APP_CONFIG_OK_RECOVERED))
    {
        ESP_LOGE(TAG, "device.json unavailable: %d (%s)",
                 (int)status, app_config_status_name(status));
        return false;
    }

    ESP_LOGI(TAG, "device.json loaded%s: product='%s' hw='%s' tb='%s'",
             (status == APP_CONFIG_OK_RECOVERED) ? " (recovered)" : "",
             device.product, device.hardware_revision,
             device.thingsboard_name);

    app_config_manufacturing_doc_t manufacturing;
    status = app_config_load_manufacturing(&manufacturing);
    if ((status != APP_CONFIG_OK) && (status != APP_CONFIG_OK_RECOVERED))
    {
        ESP_LOGE(TAG, "manufacturing.json unavailable: %d (%s)",
                 (int)status, app_config_status_name(status));
        return false;
    }

    ESP_LOGI(TAG, "manufacturing.json loaded%s: state=%d mode=%d",
             (status == APP_CONFIG_OK_RECOVERED) ? " (recovered)" : "",
             (int)manufacturing.manufacturing_state,
             (int)manufacturing.credential_mode);

    return true;
}

/**
 * @brief Load and validate the broker/TLS configuration (TASK-110).
 *
 * @return true only when the broker/TLS document was accepted and applied.
 */
static bool load_broker_tls_configuration(void)
{
    mqtt_cfg_status_t status = mqtt_cfg_load_and_apply();
    if (status != MQTT_CFG_OK)
    {
        ESP_LOGE(TAG, "Broker/TLS configuration rejected: %d (%s)"
                      " (output forced off, ThingsBoard blocked)",
                 (int)status, mqtt_cfg_status_name(status));
        return false;
    }
    return true;
}

/**
 * @brief Load the ThingsBoard access-token identity (TASK-111).
 *
 * @return true only when a validated identity is loaded and ready.
 */
static bool load_device_identity(void)
{
    device_identity_status_t status = device_identity_load();
    if (status != DEVICE_IDENTITY_OK)
    {
        ESP_LOGE(TAG, "Device identity rejected: %d (%s)"
                      " (output forced off, ThingsBoard blocked)",
                 (int)status, device_identity_status_name(status));
        return false;
    }

    ESP_LOGI(TAG, "Device identity loaded (access-token auth, client id "
                  "ready for ThingsBoard initialization)");
    return true;
}

/**
 * @brief Run the ordered configuration gates.
 *
 * @return true only when ALL THREE gates pass: product documents, the
 *         broker/TLS document and the device identity.  The state machine
 *         treats a false result as CONFIG_FAIL and parks the device in the
 *         degraded SAFE_OFF state (recovery through provisioning/reset).
 */
static bool load_configuration(void)
{
    return load_product_configuration() &&
           load_broker_tls_configuration() &&
           load_device_identity();
}

/* --------------------------------------------------------------------- */
/* Verified-TLS gate (TASK-110/111)                                        */
/* --------------------------------------------------------------------- */

/**
 * @brief Run the verified MQTT/TLS connect behind the identity gate.
 *
 * Called only in the machine's TLS state (after the network gate passed).
 * The boot order contract (TASK-111) is enforced here:
 *
 *   1. IDENTITY gate — the ThingsBoard session may only be initialized
 *      with a validated identity (device_identity_is_loaded()).  A missing
 *      or invalid identity is fatal safe-off.
 *   2. VERIFIED-TLS gate (TASK-110) — mqtt_cfg_connect() drives one
 *      verified MQTT/TLS connection over the applied transport and is the
 *      single re-enable transition for the lamp fail-off barrier.
 *
 * @return true when the verified TLS transport is established; the caller
 *         delivers TLS_CONNECTED / TLS_FAILED accordingly.
 */
static bool connect_verified_tls(void)
{
    if (!device_identity_is_loaded())
    {
        ESP_LOGE(TAG, "Device identity unavailable; ThingsBoard session "
                      "blocked, output stays off");
        return false;
    }

    mqtt_cfg_status_t status = mqtt_cfg_connect(NETWORK_CONNECT_TIMEOUT_MS);
    if (status != MQTT_CFG_OK)
    {
        ESP_LOGE(TAG, "Verified TLS connect failed: %d (%s)"
                      " (output forced off, ThingsBoard session blocked)",
                 (int)status, mqtt_cfg_status_name(status));
        return false;
    }

    /* Valid identity + verified TLS: the ThingsBoard session is now allowed
     * to initialize (TASK-112) with the access token from the token
     * provider and the stable client ID. */
    ESP_LOGI(TAG, "Verified TLS connected with validated identity; "
                  "ThingsBoard session initialization allowed");
    return true;
}

/* --------------------------------------------------------------------- */
/* State machine supervision                                              */
/* --------------------------------------------------------------------- */

/**
 * @brief One supervisor iteration: feed the watchdog, then run the gate of
 *        the current state.
 *
 * Every blocking gate polls app_state_poll() in-loop (the blocking
 * constraint of the watchdog policy: ONE task owns the feed and never
 * blocks longer than the watchdog window without polling).
 */
static void supervise_iteration(void)
{
    /* Log the parked/gate states only ONCE per state entry so a degraded
     * device does not flood the log at the supervisor cadence. */
    static app_state_t s_last_reported = APP_STATE_BOOT;
    /* Edge tracker for the exhausted-retry notice below: logs once when
     * app_state_retry_exhausted() turns true, independent of state-entry
     * reporting. */
    static bool s_last_exhausted = false;

    (void)app_state_poll(); /* feed + drive bounded retries (single owner) */

    app_state_t current = app_state_current();
    bool report_entry = (current != s_last_reported);

    switch (current)
    {
    case APP_STATE_FILESYSTEM:
        if (bootstrap_filesystem())
        {
            (void)app_state_deliver(APP_EVENT_FS_OK, APP_OWNER_FILESYSTEM);
        }
        else
        {
            (void)app_state_deliver(APP_EVENT_FS_FAIL, APP_OWNER_FILESYSTEM);
        }
        break;

    case APP_STATE_CONFIGURATION:
        if (load_configuration())
        {
            (void)app_state_deliver(APP_EVENT_CONFIG_OK,
                                    APP_OWNER_CONFIGURATION);
        }
        else
        {
            (void)app_state_deliver(APP_EVENT_CONFIG_FAIL,
                                    APP_OWNER_CONFIGURATION);
        }
        break;

    case APP_STATE_NETWORK:
        /* The network gate branches on the provisioning adapter's
         * saved-credential query (TASK-127):
         *   - credentialed device: keep the connect-only path (poll for the
         *     station, then NETWORK_CONNECTED -> TLS),
         *   - fresh device: enter PROVISIONING (PROVISIONING_STARTED ->
         *     PROVISIONING) so the portal can capture a credential. */
        if (wifi_provisioning_manager_has_saved_credentials())
        {
            if (supervise_network_gate())
            {
                (void)app_state_deliver(APP_EVENT_NETWORK_CONNECTED,
                                        APP_OWNER_NETWORK);
            }
            else
            {
                (void)app_state_deliver(APP_EVENT_NETWORK_FAILED,
                                        APP_OWNER_NETWORK);
            }
        }
        else
        {
            ESP_LOGI(TAG, "No saved station credential; entering Wi-Fi "
                          "provisioning (portal)");
            (void)app_state_deliver(APP_EVENT_PROVISIONING_STARTED,
                                    APP_OWNER_NETWORK);
        }
        break;

    case APP_STATE_PROVISIONING:
        /* Run the portal until the station connects (bounded), honor the
         * success-grace interval, then stop the portal.  On success the
         * machine returns to NETWORK, which now sees the saved credential
         * and passes to TLS; on failure it parks degraded with the bounded
         * retry (never a portal restart storm). */
        if (supervise_provisioning())
        {
            (void)app_state_deliver(APP_EVENT_PROVISIONING_SUCCEEDED,
                                    APP_OWNER_NETWORK);
        }
        else
        {
            (void)app_state_deliver(APP_EVENT_PROVISIONING_FAILED,
                                    APP_OWNER_NETWORK);
        }
        break;

    case APP_STATE_TLS:
        if (connect_verified_tls())
        {
            (void)app_state_deliver(APP_EVENT_TLS_CONNECTED, APP_OWNER_MQTT);
        }
        else
        {
            (void)app_state_deliver(APP_EVENT_TLS_FAILED, APP_OWNER_MQTT);
        }
        break;

    case APP_STATE_SYNC:
        /* No ThingsBoard desired-state synchronization is wired into the
         * product yet (TASK-112/113/114 delivered the tb_application module;
         * the transport glue and the sync gate are integrated by a later
         * task).  Until then the machine parks at the sync gate: output
         * off by the fail-off invariant and the watchdog fed.  The next
         * task calls tb_application_poll() here and delivers
         * APP_EVENT_SYNC_COMPLETE (owner APP_OWNER_THINGSBOARD) once
         * tb_application_is_synchronized() turns true. */
        if (report_entry)
        {
            ESP_LOGW(TAG, "Sync gate: no ThingsBoard client wired yet; "
                          "output off, watchdog fed");
        }
        break;

    case APP_STATE_ONLINE:
        /* All gates passed.  ThingsBoard owns the lamp state and the
         * telemetry loop from here (later task); the supervisor keeps the
         * watchdog fed and converts a Wi-Fi loss into the machine's
         * DISCONNECTED event within one cadence (the adapter already
         * forced the output off synchronously). */
        if (!network_manager_is_connected())
        {
            (void)app_state_deliver(APP_EVENT_DISCONNECTED,
                                    APP_OWNER_NETWORK);
        }
        break;

    case APP_STATE_SAFE_OFF:
        /* The bounded retry (app_state_poll) re-enters the failed stage
         * when the backoff elapses.  A parked (degraded/exhausted) device
         * waits for an explicit reset/provisioning/OTA. */
        if (!app_state_retry_pending() && report_entry)
        {
            ESP_LOGW(TAG, "Safe-off (degraded): no retry scheduled; "
                          "waiting for provisioning / reset / OTA");
        }
        /* Exhausted-park notice: log ONCE when the retry budget is spent,
         * independent of the state-entry gating above.  A device that parks
         * degraded with a retry still pending (retry_pending true) or that
         * already reported its SAFE_OFF entry would otherwise never emit a
         * clear "budget exhausted" diagnostic; the edge (false -> true)
         * keeps the cadence bounded. */
        if (app_state_retry_exhausted() && !s_last_exhausted)
        {
            ESP_LOGW(TAG, "Safe-off (degraded): retry budget exhausted; "
                          "waiting for provisioning / reset / OTA");
        }
        s_last_exhausted = app_state_retry_exhausted();
        break;

    case APP_STATE_FATAL:
        /* Never strand a provisioning listener on the shared Mongoose
         * process: retire the portal immediately on FATAL. */
        stop_provisioning_if_active();
        ESP_LOGE(TAG, "Fatal: application watchdog or unrecoverable "
                      "failure; an explicit reset is required");
        /* Leave the supervisor; a platform-level reset/OTA path reboots
         * the device. */
        return;

    case APP_STATE_OTA:
        /* OTA entry: retire the provisioning portal immediately so no portal
         * listener is left on the shared Mongoose process during the update.
         * (OTA itself is driven by a later task's OTA supervisor.) */
        stop_provisioning_if_active();
        break;

    case APP_STATE_BOOT:
    default:
        /* BOOT without a start() is parked.  Keep the watchdog fed. */
        break;
    }

    s_last_reported = current;
}

static void supervise(void)
{
    for (;;)
    {
        supervise_iteration();
        (void)osal_task_delay_ms(APP_SUPERVISE_PERIOD_MS);
    }
}

/* --------------------------------------------------------------------- */
/* Entry point                                                            */
/* --------------------------------------------------------------------- */

void app_main(void)
{
    static const app_state_config_t state_cfg = {
        .now_ms              = NULL, /* OSAL monotonic clock */
        .watchdog_timeout_ms = APP_SUPERVISE_WATCHDOG_MS,
        .on_watchdog_expired = NULL, /* FATAL + log on expiry */
        .observer            = NULL,
    };

    ESP_LOGI(TAG, "Kitchen LED Controller starting (state-machine "
                  "supervisor)");

    if (!init_lamp_output())
    {
        /* The output is off; boot continues so the filesystem bootstrap
         * (and, later, OTA state) is still available for a safe degraded
         * run.  A PWM init failure must never block storage bring-up. */
        ESP_LOGW(TAG, "Lamp output unavailable; continuing with output off");
    }

    /* Application state machine + watchdog (TASK-115).  The supervisor
     * loop below is the single watchdog-feeding task. */
    if (app_state_init(&state_cfg) != APP_STATE_OK)
    {
        ESP_LOGE(TAG, "State machine init failed; running with output off");
        return;
    }
    if (app_state_start() != APP_STATE_OK)
    {
        ESP_LOGE(TAG, "State machine start failed; running with output off");
        return;
    }

    /* Wi-Fi onboarding (TASK-109) is brought up lazily by the NETWORK
     * gate; start the manager now so the gate has a stack to wait on. */
    if (!start_network())
    {
        /* The machine is in FILESYSTEM; a network-start failure is treated
         * as a degraded boot (the supervisor delivers NETWORK_FAILED once
         * the gate runs). */
        ESP_LOGW(TAG, "Network manager not started; device stays offline");
    }

    /* Provisioning infrastructure (TASK-127).  The portal rides the shared
     * Mongoose process that also hosts MQTT/TLS; the process is initialized
     * once here and NEVER torn down by provisioning (the adapter's stop()
     * closes only the portal listeners).  The adapter itself is idempotent;
     * a failure here only disables the fresh-device portal, never the
     * credentialed boot path. */
    MongooseProcess_Init();
    if (!MongooseProcess_IsRunning())
    {
        ESP_LOGW(TAG, "Shared Mongoose process failed to start; fresh "
                      "devices will not reach the portal (credentialed "
                      "boot unaffected)");
    }
    if (wifi_provisioning_manager_init() != WIFI_PROVISIONING_MANAGER_OK)
    {
        ESP_LOGW(TAG, "Provisioning adapter init failed; fresh devices "
                      "will not reach the portal (credentialed boot "
                      "unaffected)");
    }

    supervise();
}
