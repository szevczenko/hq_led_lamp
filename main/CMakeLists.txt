# Kitchen LED Controller main application component.
#
# app_main owns the boot order (TASK-107 / TASK-115):
#   1. lamp_control_init() — output off,
#   2. the application state machine + watchdog (app_state, TASK-115)
#      starts next; the supervisor loop is the single watchdog feeder,
#   3. lamp_fs_init() — mount LittleFS via OSAL, create /cert /config /state,
#   4. configuration loading only after a successful filesystem bootstrap,
#   5. device identity (TASK-111) after configuration validation and before
#      any ThingsBoard initialization,
#   6. Wi-Fi onboarding and the ThingsBoard gates (TASK-109/110/111) behind
#      the machine's NETWORK/TLS states (the consumed
#      network_manager_is_connected() gate loop),
#   6b. Wi-Fi provisioning (TASK-127) sits between the Wi-Fi gate and TLS.
#      The platform fallback controller (TASK-131/132) owns the portal
#      policy — it opens the provisioning portal (wifi_provisioning_manager,
#      HTTP + captive DNS on the shared Mongoose process) on its own
#      decision (fresh device at init, or a credentialed device whose saved
#      credentials exhausted the bounded CONNECT_FAILED budget), keeps the
#      temporary AP up for the bounded success-grace interval
#      (CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS, the single grace
#      source of truth since TASK-131) and retires it on success.  The
#      supervisor (TASK-133) is a pure consumer: it polls the adapter's
#      outcome events (wifi_provisioning_manager_poll_event()) and maps
#      STARTED/SUCCEEDED/FAILED to the machine's PROVISIONING_STARTED/
#      SUCCEEDED/FAILED events (owner NETWORK), so PROVISIONING is entered
#      from NETWORK on the controller's fallback decision — fresh device or
#      exhausted-credential device — and control returns to the NETWORK gate
#      (now credentialed, to TLS) or parks degraded with the bounded retry.
#      A credentialed device that connects on the first try never enters
#      provisioning — it passes the NETWORK gate straight to TLS.
#      ThingsBoard STILL never starts before a verified network connection:
#      provisioning only ever hands control back to the NETWORK gate, never
#      past it.
#
# `app_state` supplies the state machine, `osal` the supervisor's
# sleep cadence (osal_task_delay_ms), and `wifi_provisioning_manager` +
# `mongoose` the provisioning portal and the shared Mongoose process that
# hosts its listeners (and later MQTT/TLS).  Kconfig-derived values
# (CONFIG_KLC_LED_PWM_GPIO etc.) come from the IDF-generated sdkconfig.h,
# which is on every component's include path.

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES lamp_control lamp_fs app_config network_manager mqtt_cfg
             device_identity app_state osal wifi_provisioning_manager mongoose
             thingsboard tb_application tb_provisioning
)
