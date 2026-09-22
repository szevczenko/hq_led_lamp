# mqtt_cfg — MQTT/TLS broker configuration (TASK-110)

Loads and validates the broker/TLS document `/config/mqtt.json` and
configures the Mongoose/ThingsBoard **verified TLS transport** through the
existing `mqtt_config` API (never direct sockets).

## Document (`/config/mqtt.json`, schema v1)

```json
{
  "schema_version": 1,
  "hostname": "home-assistance.local",
  "port": 8883,
  "tls_mode": "mqtts",
  "ca_path": "/cert/ca.crt",
  "client_cert_path": "/cert/device.crt",
  "client_key_path": "/cert/device.key",
  "client_id": "klc-kitchen-01",
  "auth_mode": "access_token",
  "skip_verify": false
}
```

- `hostname` — broker DNS name (1..128 chars); **IP literals are rejected** —
  the endpoint is a stable DNS name, never a changing raw IP.
- `port` — 1..65535 (the development endpoint is `mqtts://home-assistance.local:8883`).
- `tls_mode` — only `"mqtts"` is accepted; plaintext MQTT is rejected.
- `ca_path` — mandatory, under the logical `/cert` directory.
- `client_cert_path` / `client_key_path` — optional, under `/cert`; both
  required for `auth_mode: "mtls"`.
- `client_id` — 1..64 printable ASCII.
- `auth_mode` — `"none"` | `"access_token"` | `"mtls"`.
- `skip_verify` — optional; `true` is rejected (`mqtts` always verifies the
  private CA and the DNS hostname).

Unknown or duplicate members are rejected (strict schema).  Field/file
sizes are bounded (file ≤ 2048 bytes).

## Verified-transport guarantees

`mqtt_cfg_apply()` always configures:

- address `mqtts://<hostname>:<port>`,
- SSL enabled, skip-verify explicitly disabled (hostname verification on),
- the CA from the logical `/cert` path — resolved by the OSAL backend onto
  the LittleFS mount consistently (the component never re-bases paths),
- mTLS client cert/key only for `auth_mode: "mtls"` (cleared otherwise),
- a getter-based self-check of every applied value.

## Fail-off

Every load, validation, apply or TLS-connection failure forces the lamp
output inactive (`lamp_control_force_inactive()`) before the error status
is returned — TLS configuration failures can never enable the output.

## Tests

`tests/mqtt_cfg`:

- `mqtt_cfg_tests` — component against the real POSIX OSAL + LittleFS and
  the real `mqtt_config` codec (verified-TLS values asserted), with a
  mqtt_app double and a lamp-control double: plaintext rejection, skip
  verify rejection, empty CA path, paths outside `/cert`, missing CA file
  (invalid path), IP-literal hostname rejection, bounded fields, unknown
  schema, fail-off on config/apply/connect failure.
- `mqtt_cfg_tls_it_tests` — component end-to-end against the real Mongoose
  transport (`MG_TLS_OPENSSL`) with an in-process TLS MQTT broker: trusted
  CA success, unknown CA failure, hostname mismatch failure, plaintext
  rejection (transport never starts), invalid path rejection.