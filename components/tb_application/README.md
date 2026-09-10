# tb_application

ThingsBoard desired-state synchronization application logic (TASK-112),
server-side RPC control (TASK-113) and telemetry/health reporting
(TASK-114).

## Purpose

`power` and `brightness` shared attributes are authoritative.  This product
component turns a fresh MQTT/TLS connection into a complete, validated,
session-consistent desired state applied to the lamp:

1. subscribe to shared-attribute updates on every successful connection,
2. request `power` and `brightness` as **one** shared-attribute operation,
3. keep the lamp output off while waiting,
4. bound and validate the response (required fields, JSON types, ranges,
   session identity),
5. apply only a complete valid state — partial, duplicate, stale and late
   responses are rejected,
6. on sync timeout stay off and retry with bounded backoff,
7. reconnect restarts synchronization; disconnect always forces the lamp
   output inactive (fail-off).

On top of the synchronization contract the component owns the documented
server-side RPC surface (`setPower` / `setBrightness` / `setState` /
`getState`) as the transient service/test control channel, and the
documented telemetry/health contract (production plan section 8.3).

## Telemetry and health reporting (TASK-114)

Every successful connection, every successful state change (synchronization
apply, shared-attribute update, valid RPC set) and — periodically while
connected, rate-limited to `CONFIG_KLC_TELEMETRY_PERIOD_MS` — the module
publishes one bounded JSON record:

```json
{
  "power": false,
  "brightness": 0,
  "pwm_duty": 0,
  "connection_state": "online",
  "fw_version": "1.0.0",
  "hardware": "esp32-wroom-32d",
  "uptime_ms": 123456
}
```

- `pwm_duty` is the applied fixed-point PWM duty (`lamp_duty_from_brightness`
  on the state read back from lamp_control, in `LAMP_DUTY_SCALE` units),
  never the requested brightness alone,
- records are serialized into a bounded stack buffer
  (`TB_APPLICATION_TELEMETRY_MAX_BYTES`); an overflow suppresses the publish
  instead of emitting truncated JSON,
- `fw_version` / `hardware` come from the configuration, are truncated to
  their documented bounds and filtered to the safe charset
  `[A-Za-z0-9._+-]` before storage, so paths, quotes, whitespace, tokens and
  certificate payloads can never reach the wire,
- publication is suppressed at the source while disconnected — nothing is
  queued, buffered or retried — and the periodic publish is rate-limited to
  one record per `telemetry_period_ms`.

## Behavior reference

The module reuses the platform ThingsBoard attributes primitives
(`tb_attributes_subscribe`, `tb_attributes_request_shared`), the
connection-state observability of `tb_client` and the platform telemetry
(`tb_telemetry_send_json`) — the same primitives the platform RGB lamp
example (`platform/hq_platform/examples/common/thingboard_rgb_lamp.c`)
demonstrates.  No MQTT/TLS code lives here; the transport is owned by the
platform.

## Fail-off contract

Every rejection path — invalid types/ranges, partial data, timeout,
transport loss, duplicate data and stale callbacks — leaves the lamp output
off.  No output is enabled before a complete valid state has been
synchronized for the current connection session, and ThingsBoard stays
authoritative after every reconnect.

## Integration

```c
#include "tb_application.h"

static void on_tb_connected(tb_client_t *c, void *ud) {
    (void)ud; tb_application_on_connected(c);
}
static void on_tb_disconnected(tb_client_t *c,
                               tb_client_disconnect_reason_t r, void *ud) {
    (void)r; (void)ud; tb_application_on_disconnected(c);
}

tb_client_config_t tc = {
    .server_url = "...", .access_token = "...", .client_id = "...",
    .on_connect = on_tb_connected, .on_disconnect = on_tb_disconnected,
};
tb_client_init(&client, &tc);

tb_application_config_t cfg = {
    .client = client,
    .sync_timeout_ms = CONFIG_KLC_THINGSBOARD_SYNC_TIMEOUT_MS, /* 0 = default */
    /* retry_initial/retry_max/max_retries optional; 0 = bounded defaults */
    /* TASK-114 telemetry & health reporting: */
    .telemetry_period_ms = CONFIG_KLC_TELEMETRY_PERIOD_MS, /* 0 = default */
    .fw_version = ESP_APP_DESC_PROJECT_VERSION,            /* e.g. "1.0.0" */
    .hardware = "esp32-wroom-32d",                         /* board target */
};
tb_application_init(&cfg);

/* application loop: */
while (running) {
    tb_application_poll(client);
    osal_task_delay_ms(50);
}
```

## Tests

Host mock tests live in `tests/tb_application` and compile the real
platform `tb_attributes`/`tb_client`/`tb_rpc`/`tb_telemetry` sources against
the platform's `mqtt_app` test double (the same harness as the platform's own
`tb_tests`), with a lamp-control double:

```sh
cmake -S tests/tb_application -B tests/tb_application/build
cmake --build tests/tb_application/build
ctest --test-dir tests/tb_application/build --output-on-failure
```

Coverage: valid/partial/invalid data, oversized payloads, timeout,
duplicate updates, stale/late callbacks, bounded retry/backoff, reconnect
re-synchronization, the full server-RPC surface, and telemetry: JSON shape
and values, connect/change/periodic triggers, applied `pwm_duty`, rate
limits, disconnection suppression and secret exclusion.