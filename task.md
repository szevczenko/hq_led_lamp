# Kitchen LED Controller Development Tasks

Complete these tasks before work in `tasks-prod.md`. Each task is a standalone
implementation prompt and separate merge request.

Development baseline:

- Release target: ESP32-WROOM-32D driving an external MOSFET with PWM. S3/C6
  support must not require product-domain changes.
- Startup sequence: force LED off, mount LittleFS, load configuration, connect
  Wi-Fi, connect verified MQTT/TLS, synchronize ThingsBoard desired state, then
  enable output. Every failure returns to fail-off.
- `hq_platform` provides OSAL, LittleFS, Wi-Fi, Mongoose MQTT/TLS, and
  ThingsBoard primitives. Product code owns lamp policy and application logic.
- Development may use short-lived test certificates. Never commit or log tokens,
  passwords, provisioning secrets, certificates, or private keys.

## [ ] TASK-106 - Complete lamp-control domain component

Context:
- `lamp_control` owns validated LED policy and uses only the public OSAL PWM/GPIO API.
- State is `bool power` plus `uint8_t brightness_percent`; brightness is `0..100`.
- `power=false` is electrically off. `power=true, brightness=0` is also off but keeps requested power state. PWM polarity is compile-time configurable.

Implement the hardware-independent lamp state policy in `components/lamp_control`.

Requirements:
- define validated requested and applied lamp state,
- convert brightness to normalized PWM duty with overflow-safe integer arithmetic,
- initialize PWM at the configured inactive level before enabling it,
- expose initialize, apply-state, force-inactive, and deinitialize operations,
- reject invalid boundary input rather than wrapping it,
- add host tests for `0`, `100`, invalid values, rounding, both polarities, lifecycle failures, and fail-off.

Definition of Done:
- product code has no ESP-IDF LEDC/GPIO dependency,
- power-off and zero-brightness generate inactive output,
- output is off before initialization and after every failure path,
- lamp-control tests pass.

## [ ] TASK-107 - Add LittleFS partition and filesystem bootstrap

Context:
- LittleFS mounts at `/littlefs`; logical OSAL paths are `/cert`, `/config`, and `/state`.
- The custom partition table needs NVS, PHY, OTA data, two OTA app slots, and LittleFS. Begin LittleFS sizing at 256 KiB, then confirm against the actual WROOM flash SKU and firmware size.
- A normal mount failure must not silently format credential storage; failure leaves output off.

Create the OTA-capable partition layout and filesystem bootstrap.

Requirements:
- update `partitions.csv` for two OTA slots and appropriately sized LittleFS,
- mount through OSAL before loading configuration,
- create `/cert`, `/config`, and `/state` idempotently after successful mount,
- document when formatting is permitted and when safe failure is required,
- force lamp inactive when mount or directory creation fails,
- add host/mock tests for first mount, existing directories, mount failure, and directory failure.

Definition of Done:
- WROOM partition table validates,
- first boot creates the three directories,
- ordinary errors preserve existing storage and keep output off,
- focused filesystem tests and WROOM build pass.

## [ ] TASK-108 - Add versioned product configuration service

Context:
- Product documents are `/config/device.json` and `/config/manufacturing.json`; Wi-Fi storage remains behind an adapter.
- Every product document needs `schema_version`, file/field bounds, atomic replacement, migration hooks, and last-known-good recovery for critical configuration.
- ThingsBoard is desired-state authority; do not persist lamp state for release 1.

Implement validated, recoverable product configuration in `components/app_config`.

Requirements:
- define bounded schemas for product, hardware revision, serial, ThingsBoard name, manufacturing state, and credential mode,
- reject missing, truncated, oversized, malformed, and unknown-schema documents,
- write temporary data, flush and close, read back and validate, then atomically replace the live file where OSAL supports it,
- maintain and validate a last-known-good copy before recovery,
- expose explicit migration entry points,
- redact secrets from diagnostics,
- add POSIX LittleFS tests for success, corruption, recovery, and interrupted write.

Definition of Done:
- invalid data cannot replace valid live configuration,
- recovery uses only a validated backup,
- no lamp state is persisted,
- app-config host tests pass.

## [ ] TASK-109 - Integrate the Wi-Fi manager adapter

Context:
- Existing Wi-Fi management is a platform dependency. Product code requires a narrow adapter with connected/disconnected callbacks and a connected-state query.
- Wi-Fi onboarding precedes ThingsBoard initialization. Disconnect immediately forces output off and informs the application state machine.
- Wi-Fi credentials and Wi-Fi manager internal schema must not be exposed by product APIs or logs.

Add a product-owned adapter around the existing Wi-Fi manager.

Requirements:
- implement the documented `network_callbacks_t` interface and start/query operations,
- define thread-safe callback context ownership and late-callback behavior,
- start Wi-Fi before ThingsBoard connect attempts,
- force lamp inactive immediately in the disconnect path,
- hide Wi-Fi persistence internals from product-domain modules,
- add mocked tests for connect, disconnect, invalid credentials, reconnect, and stale callbacks.

Definition of Done:
- ThingsBoard cannot start before network connection,
- Wi-Fi loss drives fail-off within application latency,
- credentials are never logged,
- adapter tests and WROOM build pass.

## [ ] TASK-110 - Load and validate TLS MQTT configuration

Context:
- MQTT config contains broker DNS name, port, TLS mode, certificate paths, client ID, and authentication mode.
- The development endpoint is `mqtts://thingsboard.home.arpa:8883`; verify both private CA and hostname, never a changing raw IP.
- Certificate paths must remain under `/cert`; plaintext MQTT and `skip_verify=true` are invalid.

Load broker/TLS configuration and configure Mongoose/ThingsBoard verified transport.

Requirements:
- bound hostname, port, client ID, TLS mode, and path fields,
- use existing Mongoose/ThingsBoard configuration APIs rather than direct sockets,
- allow only `mqtts://` with CA and hostname verification,
- reject plaintext mode, skip verification, empty CA path, and paths outside `/cert`,
- resolve logical paths consistently through the LittleFS mount,
- force output off for configuration or TLS connection failure,
- test trusted CA, unknown CA, hostname mismatch, invalid path, and plaintext rejection.

Definition of Done:
- accepted configuration always uses verified TLS,
- TLS configuration failures cannot enable output,
- focused TLS/configuration tests pass.

## [ ] TASK-111 - Add access-token device identity

Context:
- Initial authentication is server TLS plus a unique ThingsBoard access token and stable client ID.
- Identity is loaded after filesystem/configuration validation and before ThingsBoard initialization.
- Missing or invalid identity is fatal safe-off; never fall back to anonymous or plaintext connectivity.

Implement access-token identity loading in `components/device_identity`.

Requirements:
- expose stable client-ID and token-provider interfaces,
- load only validated manufacturing/configuration records,
- bound all secret input and reject missing, empty, oversized, or malformed values,
- initialize ThingsBoard only after filesystem, TLS, network, and identity are valid,
- avoid token logging and minimize temporary secret-buffer lifetime,
- add tests for valid identity and every invalid record category.

Definition of Done:
- valid identity reaches ThingsBoard initialization through verified TLS,
- invalid identity is redacted and leaves output off,
- identity tests and WROOM build pass.

## [ ] TASK-112 - Synchronize authoritative ThingsBoard desired state

Context:
- Shared attributes `power` and `brightness` are authoritative. On each connection: subscribe, request both, keep output off, validate a complete response, then apply it.
- Invalid types/ranges, timeout, MQTT/TLS loss, duplicate data, and stale callbacks leave output off.
- Reuse existing ThingsBoard attributes/reconnect primitives and the RGB lamp example as behavioral references.

Implement connection-time desired-state synchronization in `components/tb_application`.

Requirements:
- subscribe to updates on every successful connection,
- request both attributes as one synchronization operation,
- bound payloads and validate required fields, JSON types, range, and session identity,
- apply only complete valid state and reject partial, duplicate, stale, or late responses,
- restart synchronization after reconnect with bounded retry/backoff,
- force lamp inactive on disconnect and sync timeout,
- add mock tests for valid/partial/invalid data, timeout, duplicate update, stale callback, and reconnect.

Definition of Done:
- no output is enabled before valid complete synchronization,
- ThingsBoard remains authoritative after reconnect,
- synchronization tests pass.

## [ ] TASK-113 - Add ThingsBoard RPC control

Context:
- Methods: `setPower` (`{"power": true}`), `setBrightness` (`{"brightness": 0..100}`), `setState`, and `getState`.
- RPC serves transient service/test control. Dashboards should write shared attributes unless a server rule chain explicitly synchronizes it.
- Hardware must apply before reporting success, followed by telemetry only on successful changes.

Implement validated server-side RPC control.

Requirements:
- support exactly the four documented methods,
- bound method/payload length before parsing,
- require exact JSON types, required fields, and valid brightness range,
- apply hardware state before response success,
- return structured success/error JSON for unknown method, invalid payload, and hardware failure,
- return desired and applied state from `getState`,
- add tests for valid calls and malformed, missing, wrong-type, out-of-range, unknown, and hardware failure cases.

Definition of Done:
- invalid RPC cannot modify applied state,
- success never precedes hardware application,
- RPC tests pass.

## [ ] TASK-114 - Add telemetry and health reporting

Context:
- Publish on connect, successful state change, and the configured periodic interval.
- Required fields: `power`, `brightness`, `pwm_duty`, `connection_state`, `fw_version`, `hardware`, and `uptime_ms`.
- Never publish secrets or certificate payloads. While disconnected, suppress publication rather than creating an unbounded queue.

Implement telemetry and health reporting in the ThingsBoard application component.

Requirements:
- serialize the documented fields using bounded buffers,
- report applied PWM duty, not only requested brightness,
- trigger publishes on connect/change and `CONFIG_KLC_TELEMETRY_PERIOD_MS`,
- include firmware/build version and hardware target,
- exclude tokens, passwords, provisioning secrets, certificates, keys, and full secret paths,
- rate-limit periodic telemetry and suppress it while disconnected,
- test JSON shape, values, trigger behavior, disconnection, and rate limits.

Definition of Done:
- telemetry matches the documented device contract,
- telemetry and diagnostics expose no secrets,
- telemetry tests pass.

## [ ] TASK-115 - Add application state machine and watchdog policy

Context:
- Required sequence: boot -> safe-off -> filesystem -> configuration -> Wi-Fi -> verified MQTT -> state sync -> online. OTA returns to boot; all error paths lead to safe-off/degraded state.
- Events cross Wi-Fi, MQTT, ThingsBoard, timer, OTA, and application contexts. Callback ownership and stale-session handling must be explicit.
- Retries require bounded backoff; one defined task owns watchdog feeding.

Integrate components into an explicit application state machine and watchdog policy.

Requirements:
- define states, legal transitions, events, and transition owner,
- require successful filesystem, config, Wi-Fi, TLS, and synchronization before online,
- force lamp inactive for every non-online/invalid state,
- use bounded retry/backoff for recoverable failures,
- reject stale callbacks with a generation/session identity,
- define watchdog owner, feed points, timeout behavior, and blocking constraints,
- add fault-injection tests for each failure transition, reconnect loop, timeout, watchdog recovery, and OTA entry/exit.

Definition of Done:
- all failure paths force inactive output,
- retry cannot cause connection storms or callback races,
- state-machine tests and WROOM build pass.

## [ ] TASK-116 - Deploy the development LAN ThingsBoard stack

Context:
- Development topology is controller -> Wi-Fi LAN -> `mqtts` port 8883 on Ubuntu -> ThingsBoard; administration uses HTTPS 443.
- Use `thingsboard.home.arpa` with DHCP reservation/local DNS. Keep it LAN-only and do not port-forward it.
- This task enables development integration testing. Production CA custody, hardening, backup evidence, and release sign-off are in `tasks-prod.md`.

Create reproducible development server assets under `server`.

Requirements:
- add pinned Docker Compose ThingsBoard/PostgreSQL services with durable development storage,
- include non-secret environment template and ignore real environment/secrets files,
- configure development HTTPS and MQTT TLS at `thingsboard.home.arpa:8883`,
- disable plaintext MQTT or bind it to localhost only,
- document DNS/DHCP, startup, shutdown, reset, and integration-test setup,
- add health checks and a credential-free endpoint validation script,
- ignore generated development certificate and database paths.

Definition of Done:
- a clean development environment deploys from documentation,
- data survives container recreation,
- HTTPS and verified MQTT TLS endpoints are reachable,
- no private material is committed.

## [ ] TASK-117 - Add development CA and production PKI procedures

Context:
- Public ACME cannot normally validate private `home.arpa` names. Development uses a short-lived private CA; devices install only its root at `/cert/ca.crt`.
- Server certificates require `DNS:thingsboard.home.arpa` SAN and hostname verification.
- Production needs a protected/offline root process where possible; its private key stays off the ThingsBoard runtime host. Development roots never ship in production artifacts.

Automate development certificates and document the production PKI boundary.

Requirements:
- add scripts for a development root CA, server key/CSR, and server certificate,
- require the documented DNS SAN,
- document key usage, permissions, lifetime, renewal, and device trust-store installation,
- keep generated certificates/private keys in ignored directories,
- document protected production CA custody and renewal without generating production material,
- validate trusted CA success, unknown CA failure, and hostname mismatch,
- verify no device receives CA or server private keys.

Definition of Done:
- generated key/certificate material is ignored by Git,
- development certificate verifies for the documented host,
- expected TLS success/failure checks pass,
- production CA custody and renewal are documented.