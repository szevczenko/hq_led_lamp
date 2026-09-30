# device_identity — access-token device identity (TASK-111)

Loads the ThingsBoard access token and the stable MQTT client ID from the
validated identity record `/config/identity.json` and exposes them through
the stable client-ID and token-provider interfaces.  Authentication is
**verified server TLS** (owned by `mqtt_cfg`, TASK-110) **plus** this
identity — a missing or invalid identity is fatal for ThingsBoard
initialization and never falls back to anonymous or plaintext connectivity.

## Identity record (`/config/identity.json`, schema v1)

```json
{
  "schema_version": 1,
  "client_id": "klc-kitchen-01",
  "access_token": "YOUR_THINGSBOARD_ACCESS_TOKEN"
}
```

- `schema_version` — must equal `1` (newer versions are rejected as
  `unknown_schema`; older versions have no migration path in release 1).
- `client_id` — the stable MQTT client ID, 1..64 printable ASCII.  The
  manufacturing flow writes the **same value** into `/config/mqtt.json`, so
  the transport and the ThingsBoard client agree on one stable identifier.
- `access_token` — the ThingsBoard device credential (MQTT username),
  1..128 printable ASCII without whitespace (spaces/newlines/control
  characters are rejected).

Unknown or duplicate members are rejected (strict schema); the whole input
must be consumed (trailing garbage or a second concatenated object is
rejected); file size is bounded (≤ 2048 bytes).

The provisioning secret is deliberately **not** stored in the `app_config`
managed documents (`manufacturing.json` never carries secrets); this
component owns its own bounded record so the token is validated and
versioned independently.

## Manufacturing/configuration gate

`device_identity_load()` only trusts validated records:

1. the manufacturing document is loaded through `app_config`
   (`app_config_load_manufacturing()`) and must report
   `manufacturing_state == PROVISIONED` with `credential_mode != NONE`,
   otherwise `DEVICE_IDENTITY_ERR_UNPROVISIONED` is returned and the
   secret record is never read,
2. the identity record is then loaded, parsed with the strict v1 schema and
   validated field-by-field.

## Fail-off and ordering

- Runs after the filesystem bootstrap and the product configuration
  validation, and before any ThingsBoard initialization.
- Every load failure forces the lamp output inactive
  (`lamp_control_force_inactive()`), leaves the module unloaded, and blocks
  ThingsBoard initialization (`device_identity_is_loaded()` is the gate).
- A successful load does **not** release the lamp fail-off barrier — the
  single re-enable transition stays owned by the verified MQTT/TLS connect
  (`mqtt_cfg_connect()`, TASK-110).

## Secret handling

- The token is **never logged**: this component performs no logging at all,
  and `device_identity_status_name()` returns only statuses (no record
  content).  Callers must not log the token or echo it in diagnostics.
- Temporary secret lifetime is minimized: the raw record buffer and the JSON
  parser's value copy are zeroized before being freed, so after a successful
  load exactly one copy remains (module-owned storage), and
  `device_identity_clear()` zeroizes it (lifecycle end / factory reset).
- `device_identity_token()` and the token-provider callback return that
  module-owned value; consumers copy it into the ThingsBoard client
  configuration and must not log it.

## Layout

- `include/device_identity.h` — normative public API and contract.
- `device_identity.c` — implementation.
- `CMakeLists.txt` — ESP-IDF component registration
  (`PRIV_REQUIRES osal json app_config lamp_control`).
- `../tests/device_identity` — host tests.

## Host tests

`tests/device_identity` compiles the production source (device_identity +
app_config) against the **real POSIX OSAL backend on a temporary
file-backed LittleFS volume**, the vendored cJSON, a lamp-control double
and the Unity framework:

- valid identity → `DEVICE_IDENTITY_OK`; the stable client-ID and
  token-provider interfaces return the validated values verbatim,
- every invalid record category → distinct rejection and fail-off:
  missing record, unprovisioned/quarantined/credential-less manufacturing
  record, missing member, empty client ID / token, oversized file / field,
  malformed JSON, trailing garbage, unknown member, duplicate member,
  control characters, unknown schema, wrong types,
- fail-off: every rejection forces `lamp_control_force_inactive()` exactly
  once and leaves the module unloaded (getters return `not_loaded`),
- redaction: rejection diagnostics carry no token (status names only; the
  component never logs),
- zeroization: `device_identity_clear()` invalidates the identity and wipes
  the module token buffer.

```sh
cmake -S tests/device_identity -B build-device-identity-tests
cmake --build build-device-identity-tests
ctest --test-dir build-device-identity-tests --output-on-failure
```