/**
 * @file app_main.c
 * @brief Kitchen LED Controller application entry point.
 *
 *  ## Boot-order contract (TASK-107 / TASK-118 / TASK-109 / TASK-115 /
 *                        TASK-127 / TASK-136)
 *
 *   1. lamp_control_init() — the output is initialized off and stays off
 *      until a valid desired state arrives.
 *   2. The application state machine (app_state, TASK-115) is started
 *      next: it is the explicit owner of the sequence
 *      boot -> safe-off -> filesystem -> configuration -> Wi-Fi ->
 *      provisioning (entered from NETWORK on the controller's fallback
 *      decision — fresh device or exhausted-credential device) ->
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
 *   7. Wi-Fi onboarding (TASK-109) runs at the machine's NETWORK gate,
 *      through the product-owned network adapter.  TASK-136: the manager
 *      (and the provisioning infrastructure of 7b) is brought up on the
 *      gate's FIRST entry — after the FILESYSTEM gate mounted the LittleFS
 *      storage and the CONFIGURATION gate passed — so the manager's
 *      init-time load of saved station credentials (wifi_ap.json) reads
 *      from the mounted volume, wifi_mgmt_is_read_data() reflects reality,
 *      and the controller's fresh-device decision (7b) sees the truth.
 *      The gate itself is the consumed network_manager_is_connected()
 *      polling loop below (the successor of
 *      network_manager_wait_connected()): ThingsBoard must never connect
 *      before this gate passes.  A Wi-Fi loss forces the adapter to fail
 *      the lamp off synchronously; the supervisor converts the disconnect
 *      into the machine's DISCONNECTED event within one supervisor
 *      cadence.
 *   7b. Wi-Fi provisioning (TASK-127) sits between the Wi-Fi gate and TLS.
 *      The platform fallback controller (TASK-131/132,
 *      CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK=y with a bounded
 *      CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS budget) owns the
 *      portal policy: it opens the provisioning portal (HTTP + captive DNS
 *      on the shared Mongoose process) on its own decision — a fresh device
 *      (no saved station credential) at init, or a credentialed device
 *      whose saved credentials exhausted the CONNECT_FAILED budget — keeps
 *      the temporary AP up for the bounded success-grace interval
 *      (CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS, the single grace
 *      source of truth since TASK-131; the duplicate KLC knob was dropped)
 *      and retires it on success.  The supervisor (TASK-133) is a pure
 *      consumer of the adapter's outcome events
 *      (wifi_provisioning_manager_poll_event()): PROVISIONING is entered
 *      from NETWORK on the controller's fallback decision — fresh device or
 *      exhausted-credential device — when the supervisor delivers
 *      PROVISIONING_STARTED (owner APP_OWNER_NETWORK), and
 *      PROVISIONING_SUCCEEDED hands control back to the NETWORK gate (which
 *      passes the now credentialed station to TLS) or PROVISIONING_FAILED
 *      parks the machine degraded (SAFE_OFF with the existing bounded
 *      retry).  The supervisor never starts or restarts the portal.  A
 *      credentialed device that connects on the first try never enters
 *      provisioning — it passes the NETWORK gate straight to TLS.
 *      ThingsBoard STILL never starts before a verified network
 *      connection: provisioning only ever hands control back to the
 *      NETWORK gate, never past it.
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
 *      - the provisioning outcome pump
 *        (wifi_provisioning_manager_poll_event()) is non-blocking and runs
 *        in the same supervisor task, one iteration per poll — the portal is
 *        owned by the platform fallback controller, and the adapter's
 *        notification path only STORES events (never blocks on the Mongoose
 *        /Wi-Fi callback contexts); the supervisor DELIVERS them
 *        (TASK-133),
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

/* --------------------------------------------------------------------- */
/* Lazy Wi-Fi/provisioning bring-up (TASK-136)                             */
/* --------------------------------------------------------------------- */

/**
 * @brief One-shot guard for the NETWORK gate's first-run bring-up.
 *
 * The Wi-Fi manager and the provisioning adapter are initialized on the
 * FIRST entry of the machine's NETWORK gate — never before it (see the
 * boot-order contract above).  By then the FILESYSTEM gate has mounted the
 * LittleFS storage, so the manager's init-time load of saved station
 * credentials (wifi_mgmt_init -> _load_saved_config -> wifi_ap.json) reads
 * from the mounted volume: a device with a saved credential passes the gate
 * straight to TLS and a portal submission (SUCCEEDED -> NETWORK) passes the
 * now-credentialed station to TLS without a reboot.
 */
static bool s_network_bringup_done;

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
/* Provisioning outcome events (TASK-132/133)                              */
/* --------------------------------------------------------------------- */

/**
 * @brief Stop the provisioning flow through the adapter (idempotent).
 *
 * Ends the controller's portal lifecycle (any pending success-grace timer is
 * cancelled and the temporary AP retirement is requested —
 * wifi_provisioning_controller_stop() inside the adapter) and closes any
 * remaining portal listeners.  The shared Mongoose process (and with it
 * MQTT/TLS) is never torn down.  Safe to call at any time — the adapter
 * stop() is idempotent and a no-op when the portal is already stopped.  Used
 * to guarantee the portal never strands a listener or a pending grace timer
 * on OTA entry or FATAL.
 */
static void stop_provisioning(void)
{
    (void)wifi_provisioning_manager_stop();
}

/**
 * @brief Deliver pending provisioning outcome events (TASK-133).
 *
 * The platform fallback controller owns the portal policy (TASK-131/132);
 * the adapter's notification path only STORES the controller's state-change
 * notifications (never blocks on a Mongoose/Wi-Fi callback context), and
 * this supervisor task DELIVERS them into the state machine with the current
 * session identity:
 *
 *   - STARTED   -> APP_EVENT_PROVISIONING_STARTED   (owner NETWORK),
 *   - SUCCEEDED -> APP_EVENT_PROVISIONING_SUCCEEDED (owner NETWORK),
 *   - FAILED    -> APP_EVENT_PROVISIONING_FAILED    (owner NETWORK).
 *
 * The machine may enter PROVISIONING ONLY from NETWORK, and the
 * SUCCEEDED/FAILED outcomes are only meaningful from PROVISIONING.
 * wifi_provisioning_manager_poll_event() CONSUMES the head of the adapter's
 * FIFO, so this pump polls ONLY while the machine is at the NETWORK or
 * PROVISIONING gate and holds the event otherwise: a fresh device's
 * controller fires STARTED at init while the machine is still in
 * FILESYSTEM/CONFIGURATION, and polling from there would drain and drop it
 * before the machine ever reaches the NETWORK gate (permanently wedging the
 * device outside the portal flow).  Holding the head (by not polling)
 * preserves the adapter's FIFO order with any following SUCCEEDED/FAILED.
 * SUCCEEDED/FAILED only ever sit behind a consumed STARTED (the controller
 * opens the portal before it can report an outcome), so gating on these two
 * states cannot strand them, and a delivery from any other state is
 * rejected (dropped and counted) by the machine itself as stale/illegal —
 * the machine owns that decision.
 *
 * Non-blocking: no wait loop and no blocking call, so the single watchdog
 * owner is preserved and the loop never blocks from a callback context.
 */
static void deliver_provisioning_events(void)
{
    for (;;)
    {
        const app_state_t state = app_state_current();

        /* Deliver STARTED only at the NETWORK gate and the outcomes only at
         * the PROVISIONING gate: poll_event() CONSUMES the event, so the
         * machine-state check must happen FIRST — a poll from any other
         * state (BOOT/FILESYSTEM/CONFIGURATION/SAFE_OFF/...) would drop the
         * head STARTED permanently. */
        if ((state != APP_STATE_NETWORK) && (state != APP_STATE_PROVISIONING))
        {
            return;
        }

        wifi_provisioning_manager_event_t event =
            wifi_provisioning_manager_poll_event();

        switch (event)
        {
        case WIFI_PROVISIONING_MANAGER_EVENT_STARTED:
            if (state != APP_STATE_NETWORK)
            {
                /* A STARTED at the PROVISIONING gate is a stale duplicate
                 * (the machine is already provisionable): drop it — the
                 * machine rejects PROVISIONING_STARTED from any non-NETWORK
                 * state as illegal, so consuming here is safe. */
                continue;
            }
            ESP_LOGI(TAG, "Provisioning flow started by the controller; "
                          "entering Wi-Fi provisioning");
            (void)app_state_deliver(APP_EVENT_PROVISIONING_STARTED,
                                    APP_OWNER_NETWORK);
            continue;

        case WIFI_PROVISIONING_MANAGER_EVENT_SUCCEEDED:
            ESP_LOGI(TAG, "Provisioning succeeded; portal retired by the "
                          "controller; NETWORK gate resumes");
            (void)app_state_deliver(APP_EVENT_PROVISIONING_SUCCEEDED,
                                    APP_OWNER_NETWORK);
            continue;

        case WIFI_PROVISIONING_MANAGER_EVENT_FAILED:
            ESP_LOGW(TAG, "Provisioning failed; machine parks degraded "
                          "(bounded retry)");
            (void)app_state_deliver(APP_EVENT_PROVISIONING_FAILED,
                                    APP_OWNER_NETWORK);
            continue;

        case WIFI_PROVISIONING_MANAGER_EVENT_NONE:
        default:
            return;
        }
    }
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

    /* Provisioning outcome events (TASK-132/133): the platform fallback
     * controller owns the portal, the adapter stores its notifications and
     * the supervisor DELIVERS them here (current session identity, machine
     * owns the stale/illegal rejection).  Non-blocking; same task as the
     * feed. */
    deliver_provisioning_events();

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
        /* First NETWORK entry — the FILESYSTEM and CONFIGURATION gates have
         * already passed (machine-order contract), so the LittleFS storage
         * is mounted: the Wi-Fi manager's init-time saved-credential load
         * (wifi_mgmt_init -> _load_saved_config -> wifi_ap.json) reads the
         * TRUTHFUL storage contents and wifi_mgmt_is_read_data() reflects
         * reality.  TASK-136: bringing the network up here (instead of in
         * app_main before the FILESYSTEM gate) is what lets a device with a
         * saved credential pass this gate straight to TLS, and lets a portal
         * submission (SUCCEEDED -> NETWORK) pass the now-credentialed
         * station to TLS without a reboot. */
        if (!s_network_bringup_done)
        {
            s_network_bringup_done = true;
            if (!start_network())
            {
                /* Degraded boot: the manager never started, so there is
                 * nothing to poll.  Fail the gate now; the machine parks
                 * SAFE_OFF (the bounded retry re-enters the network stage,
                 * never provisioning directly). */
                (void)app_state_deliver(APP_EVENT_NETWORK_FAILED,
                                        APP_OWNER_NETWORK);
                break;
            }

            /* Provisioning infrastructure (TASK-127).  The portal rides the
             * shared Mongoose process that also hosts MQTT/TLS; the process
             * is initialized once here and NEVER torn down by provisioning
             * (the adapter's stop() ends the controller lifecycle and closes
             * only the portal listeners).  init() registers the product
             * notification hook with the platform fallback controller
             * (TASK-132) — now that the manager loaded the saved-credential
             * state, the controller's fresh-device decision sees the truth.
             * The adapter itself is idempotent; a failure here only disables
             * the fresh-device portal, never the credentialed boot path. */
            MongooseProcess_Init();
            if (!MongooseProcess_IsRunning())
            {
                ESP_LOGW(TAG, "Shared Mongoose process failed to start; "
                              "fresh devices will not reach the portal "
                              "(credentialed boot unaffected)");
            }
            if (wifi_provisioning_manager_init() !=
                WIFI_PROVISIONING_MANAGER_OK)
            {
                ESP_LOGW(TAG, "Provisioning adapter init failed; fresh "
                              "devices will not reach the portal "
                              "(credentialed boot unaffected)");
            }
        }
        /* The network gate keeps ONLY the credentialed connect path
         * (TASK-133): poll for the station within the bounded window, then
         * NETWORK_CONNECTED -> TLS (or NETWORK_FAILED -> SAFE_OFF with the
         * bounded retry).  A device without a usable saved credential never
         * starts provisioning from here — the platform fallback controller
         * owns that decision (fresh device at init, or exhausted
         * CONNECT_FAILED budget) and the supervisor delivers its STARTED
         * event above while the machine is in NETWORK. */
        {
            const bool credentialed =
                wifi_provisioning_manager_has_saved_credentials();
            if (credentialed && supervise_network_gate())
            {
                (void)app_state_deliver(APP_EVENT_NETWORK_CONNECTED,
                                        APP_OWNER_NETWORK);
            }
            else if (credentialed)
            {
                (void)app_state_deliver(APP_EVENT_NETWORK_FAILED,
                                        APP_OWNER_NETWORK);
            }
        }
        break;

    case APP_STATE_PROVISIONING:
        /* The controller owns the portal (TASK-131/132/133): the machine
         * parks in this bookkeeping state while the controller keeps the
         * temporary AP / HTTP / DNS up.  No portal-driving work happens
         * here — the pump above delivers SUCCEEDED (-> NETWORK) or FAILED
         * (-> SAFE_OFF, bounded retry) when the controller reports the
         * outcome, and the portal is never restarted from the supervisor.
         * The watchdog is fed every iteration by the loop above. */
        if (report_entry)
        {
            ESP_LOGI(TAG, "Provisioning gate: portal owned by the "
                          "controller; waiting for its outcome events");
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
        /* Never strand the provisioning flow on FATAL: the adapter stop
         * ends the controller lifecycle (grace timer cancelled, temporary AP
         * retired) and closes any portal listeners on the shared Mongoose
         * process. */
        stop_provisioning();
        ESP_LOGE(TAG, "Fatal: application watchdog or unrecoverable "
                      "failure; an explicit reset is required");
        /* Leave the supervisor; a platform-level reset/OTA path reboots
         * the device. */
        return;

    case APP_STATE_OTA:
        /* OTA entry: end the provisioning flow immediately so no portal
         * listener or pending grace timer is left during the update.
         * (OTA itself is driven by a later task's OTA supervisor.)  The
         * adapter stop is idempotent; this runs once per OTA entry. */
        if (report_entry)
        {
            stop_provisioning();
        }
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

    /* Wi-Fi onboarding (TASK-109) and the provisioning infrastructure
     * (TASK-127) are brought up lazily by the NETWORK gate on its first
     * entry (TASK-136): the supervisor loop runs the FILESYSTEM gate
     * (LittleFS mount) and the CONFIGURATION gate before the NETWORK gate,
     * so the Wi-Fi manager's init-time saved-credential load
     * (wifi_mgmt_init -> _load_saved_config) reads from the mounted storage
     * and the platform controller's fresh-device decision sees the truth.
     * Starting the manager here would load nothing (the OSAL file layer
     * rejects unmounted access) and would wedge the NETWORK gate forever
     * after a portal submission — see the boot-order contract above. */
    supervise();
}
