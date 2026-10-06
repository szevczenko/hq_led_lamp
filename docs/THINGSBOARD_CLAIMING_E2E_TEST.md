# E2E: Claiming integration scenario on ThingsBoard PE

> **Status: NOT PASSED — this scenario requires a ThingsBoard PE environment and a physical lamp.**
>
> Nothing in this document is a passed result. The scenario below has **not** been executed on the
> target edition/version; it is a complete, runnable integration test to be performed on
> **ThingsBoard PE** with a **physical lamp**. Every ThingsBoard-dependent statement is tagged with
> its source:
>
> * `[PE docs]` — ThingsBoard PE documentation (https://thingsboard.io/docs/pe/user-guide/claiming/),
> * `[endpoint]` — a concrete ThingsBoard REST endpoint (see the ThingsBoard REST API reference,
>   https://thingsboard.io/docs/reference/rest-api/),
> * `[§9.5]` — `FIRMWARE_PRODUCTION_READINESS_PLAN.md` §9.5 (this repository),
> * `[manual §N]` — `docs/THINGSBOARD_USER_MANUAL.md` section N (this repository),
> * `[TASK-302]` / `[TASK-303]` — `scripts/thingsboard/claim_handover.sh` / `claim_customer.sh`
>   (this repository; behavior verified in the checkout),
> * `[repo]` — a verified repository fact (file path given),
> * `[test]` — a check **to be run** in this scenario; the expected outcome is an assumption until
>   the run proves it on PE with a physical lamp.

---

## 1. Scope

This scenario covers the complete server-side claiming lifecycle (chosen method documented in
`docs/THINGSBOARD_USER_MANUAL.md` §13, `[manual §13]`), end to end on the test instance:

1. TASK-302 tenant-side preparation — write `claimingData` (`claim_handover.sh`).
2. Customer / Customer User fixture per the §9.5 template.
3. TASK-303 Customer User claim/reclaim (`claim_customer.sh`): correct claim, wrong claim,
   expired claim, retry after a successful claim, reclaim to the tenant.
4. Post-claim firmware behavior on a physical lamp: reboot + continued MQTT, Customer User access
   to RPC / telemetry / attributes, factory reset identity behavior.
5. Secret hygiene: the claiming secret never appears in device logs or server-side logs/script output.

The scenario is **not** a proof of the target PE behavior on its own: the development smoke tests
and this run's results on the test instance are smoke tests only (`[manual §13.6]`). The
PE-specific expectations are collected in the "To be confirmed on PE" list (§8), which covers
assumptions 1–5 of the implementation plan.

## 2. Test environment and security rules

* Test instance base URL (all API checks): `http://home-assistance.local:8080` — a local
  development/test ThingsBoard instance, **not** production.
* Tenant-admin access: an API key with `TENANT_ADMIN` rights, read **at runtime** from
  `/home/dima/projects/hq_workspace/thingboard_tenant_api_key` (one key, whitespace stripped when
  read). The key value is **never** written into this document, scripts, logs, or task files —
  every example below uses a runtime read placeholder (`$(cat ...)` / `$(tr -d '[:space:]' < ...)`),
  never the literal key.
* Customer User actions (claim/reclaim and post-claim access checks) **cannot** be performed with
  the tenant API key — they require a Customer User login (JWT) as in §9.5 (`[§9.5]`, `[TASK-303]`).
* The tenant API key grants tenant-admin rights: use it only for development checks on the test
  instance, never against production.
* Firmware behavior is verified by running the monitor flow, capturing
  `/tmp/hq_led_lamp_esp.log`, and reading/grepping the **actual** log — never by assuming behavior.
* The claiming secret and the MQTT access token are distinct credentials; the provisioning
  device key/secret and the MQTT access token are **not** the claiming secret (`[manual §13.5]`).

## 3. Prerequisites

* A **ThingsBoard PE** environment (target version ≥ 3.5/3.6 — assumption 1, §8) reachable at
  `http://home-assistance.local:8080` with a tenant-admin API key in
  `/home/dima/projects/hq_workspace/thingboard_tenant_api_key`.
* A **physical lamp** flashed with the current firmware, provisioned as a tenant device
  (`[manual §2.1]`), connected to the Wi-Fi network, and reachable on the serial port
  `/dev/ttyUSB1`.
* Tooling: `curl`, `jq`, `openssl`, `date`, `sed`, `tr`, `bash`; ESP-IDF environment exported for
  the monitor flow (`source /home/dima/projects/esp-idf/export.sh`).
* A dedicated E2E device name to avoid touching production devices, e.g. `klc-claim-e2e-01`
  (created via normal provisioning, `[manual §2.1]`).

## 4. Test fixture: Customer + Customer User (per the §9.5 template)

The tenant admin creates the test Customer and Customer User exactly as in `[§9.5]`, adapted to the
test instance and the runtime key read. The key is read at runtime only; the value never appears in
this document.

```bash
export TB_BASE_URL="http://home-assistance.local:8080"
# Runtime read of the tenant API key (placeholder — never the literal key):
TB_API_KEY="$(tr -d '[:space:]' < /home/dima/projects/hq_workspace/thingboard_tenant_api_key)"
tb() { curl -sS -H "Content-Type: application/json" -H "X-Authorization: ApiKey $TB_API_KEY" "$@"; }

# Customer + Customer User (tenant)  [endpoint: POST /api/customer, POST /api/user,
#                                     GET /api/user/{id}/activationLink, POST /api/noauth/activate]
CUST_ID="$(tb -X POST "$TB_BASE_URL/api/customer" \
  -d '{"title":"KLC Test Customer A"}' | jq -r .id.id)"
USER_ID="$(tb -X POST "$TB_BASE_URL/api/user?sendActivationMail=false" -d '{
  "email":"klc.test.a@example.com","authority":"CUSTOMER_USER",
  "customerId":{"id":"'$CUST_ID'","entityType":"CUSTOMER"}}' | jq -r .id.id)"
ACT="$(tb "$TB_BASE_URL/api/user/$USER_ID/activationLink" | sed 's/.*activateToken=//')"
read -rs -p "Customer password: " CUST_PASS; echo
curl -sS -X POST "$TB_BASE_URL/api/noauth/activate" -H "Content-Type: application/json" \
  -d '{"activateToken":"'$ACT'","password":"'$CUST_PASS'"}' >/dev/null
```

* Expected result: the Customer and the Customer User exist and the user is activated
  (`[§9.5]`, `[test]`).
* Alternative (scripted, same endpoints): `scripts/thingsboard/claim_customer.sh setup-test-customer`
  creates/reuses the test Customer + Customer User with the tenant API key read from
  `TB_TENANT_API_KEY_FILE` (default: `/home/dima/projects/hq_workspace/thingboard_tenant_api_key`)
  (`[TASK-303]`). With `TB_CUSTOMER_PASSWORD` set, the user is activated programmatically; without
  it, the activation link (a credential — do not log it) is printed to stdout (`[TASK-303]`).

## 5. Baseline (before claiming)

1. Confirm the lamp is online under the tenant and behaves normally before any claiming step.
2. Capture the baseline device log with the monitor flow (starting the monitor resets the target,
   which also serves as the reboot check):

   ```bash
   cd /home/dima/projects/hq_workspace/hq_led_lamp
   source /home/dima/projects/esp-idf/export.sh
   timeout 1m idf.py -p /dev/ttyUSB1 monitor | tee /tmp/hq_led_lamp_esp.log
   ```

3. Read/grep the **actual** log for the expected baseline evidence (`[manual §7]`, `[repo:
   main/app_main.c]`):

   ```bash
   grep -E "Firmware: title=" /tmp/hq_led_lamp_esp.log
   grep -E "mqtt_cfg: verified transport configured" /tmp/hq_led_lamp_esp.log
   grep -E "Verified TLS connected with validated identity" /tmp/hq_led_lamp_esp.log
   grep -E "sync source=" /tmp/hq_led_lamp_esp.log
   grep -E "sync-complete" /tmp/hq_led_lamp_esp.log
   ```

   Record the actual lines found; do not assume them.
4. Tenant-side sanity check of the same device (`[endpoint: GET /api/tenant/devices]`):

   ```bash
   export DEV_NAME="klc-claim-e2e-01"
   DEV_ID="$(tb "$TB_BASE_URL/api/tenant/devices?deviceName=$DEV_NAME" | jq -r .id.id)"
   echo "device id: $DEV_ID"
   tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/timeseries?keys=connection_state"
   ```

   Expected: `connection_state=online` (`[test]`).

## 6. E2E scenario steps

All steps below run against `http://home-assistance.local:8080`. The tenant API key is read at
runtime from `/home/dima/projects/hq_workspace/thingboard_tenant_api_key` (placeholder only in this
document); Customer User steps use the Customer User JWT, never the tenant API key.
The steps assume one continuous shell session from §4/§5, so the helpers and variables defined there
(`tb`, `TB_BASE_URL`, `DEV_NAME`, `DEV_ID`, `CUST_PASS`) and the memory-only `SECRET_VAL` from §6.4
remain available. Secrets live only in shell variables / stdin pipes and are never written to files.

### 6.1 Tenant writes `claimingData` (TASK-302 step)

```bash
# Issue (or re-issue) claimingData for the E2E device; TTL 600 s for the test.
scripts/thingsboard/claim_handover.sh "$DEV_NAME" --ttl 600
scripts/thingsboard/claim_handover.sh "$DEV_NAME" --status
```

* What happens (`[TASK-302]`): generates a crypto-random one-time secret (`openssl rand -hex 32`),
  writes `claimingData` as a SERVER_SCOPE attribute via
  `POST /api/plugins/telemetry/DEVICE/{deviceId}/attributes/SERVER_SCOPE`
  (`[endpoint]`), body `{"claimingData":"{\"secretKey\":\"...\",\"expirationTime\":<ms>}"}`
  (`[§9.5]`, `[PE docs]`).
* The secret is printed to stdout **only** in explicit `--handover` mode; `--status` shows the
  expiration but never the secret (`[TASK-302]`).
* Expected: `claimingData: issued for device ...` and `--status` shows a future expiration
  (`[test]`).

### 6.2 Wrong claim (Customer User) — rejected

```bash
# Deliberately wrong secret (placeholder — wrong on purpose):
export TB_CUSTOMER_USERNAME="klc.test.a@example.com"
export TB_CUSTOMER_PASSWORD="$CUST_PASS"
export TB_CLAIM_SECRET="<deliberately-wrong-secret>"
scripts/thingsboard/claim_customer.sh "$DEV_NAME" claim
echo "exit=$?"   # expected 3
unset TB_CLAIM_SECRET
```

* Endpoint: `POST /api/customer/device/{deviceName}/claim` with `{"secretKey":"..."}`
  (`[endpoint]`, `[PE docs]`).
* Expected: exit code 3 (wrong secret) — the claim is rejected and the device stays with the
  tenant (`[TASK-303]`, `[test]`).

### 6.3 Expired claim — rejected

Write `claimingData` with an `expirationTime` in the past (tenant admin; the secret value is
irrelevant because expiry is checked first — the check order is a PE behavior to record,
`[test]`):

```bash
EXPIRED_MS=$(( ($(date +%s) - 60) * 1000 ))
tb -X POST "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/attributes/SERVER_SCOPE" \
  -d '{"claimingData":"{\"secretKey\":\"<one-time-secret>\",\"expirationTime\":'$EXPIRED_MS'}"}'

export TB_CLAIM_SECRET="<one-time-secret>"
scripts/thingsboard/claim_customer.sh "$DEV_NAME" claim
echo "exit=$?"   # expected 4
unset TB_CLAIM_SECRET
```

* Endpoint: `POST /api/plugins/telemetry/DEVICE/{deviceId}/attributes/SERVER_SCOPE` (write) and
  `POST /api/customer/device/{deviceName}/claim` (claim) (`[endpoint]`).
* Expected: exit code 4 (claiming data expired) (`[TASK-303]`, `[test]`). If the instance reports
  a different reason first (e.g. wrong secret), record the actual check order — that is a PE
  behavior detail (`[test]`).

### 6.4 Correct claim — success

```bash
# Issue a fresh secret and keep it in a shell variable (memory only, never a file):
SECRET_VAL="$(scripts/thingsboard/claim_handover.sh "$DEV_NAME" --ttl 600 --handover 2>/dev/null)"
printf '%s' "$SECRET_VAL" | scripts/thingsboard/claim_customer.sh --secret-stdin "$DEV_NAME" claim
echo "exit=$?"   # expected 0
```

* Endpoint: `POST /api/customer/device/{deviceName}/claim` with the correct `{"secretKey":"..."}`
  (`[endpoint]`, `[PE docs]`).
* Expected: exit 0, `claim: device ... claimed by customer user ...`; the device is transferred to
  the Customer (`[TASK-303]`, `[test]`).
* Verify assignment (tenant admin): `GET /api/customer/{customerId}/devices?deviceName=...` lists
  the device, or the device entity shows the Customer assignment (`[endpoint]`, `[test]`).

### 6.5 Retry after a successful claim — rejected

```bash
printf '%s' "$SECRET_VAL" | scripts/thingsboard/claim_customer.sh --secret-stdin "$DEV_NAME" claim
echo "exit=$?"   # expected 5
```

* Expected: exit code 5 (device already claimed) — the claim is one-time and the secret is
  consumed (`[TASK-303]`, `[PE docs]`, `[test]`).

### 6.6 Lamp reboot + continued MQTT (post-claim, no firmware action)

The device is now claimed by the Customer. Reboot the physical lamp with the monitor flow and
verify it reconnects **with the same MQTT credentials** to the **same device entity**:

```bash
cd /home/dima/projects/hq_workspace/hq_led_lamp
source /home/dima/projects/esp-idf/export.sh
timeout 1m idf.py -p /dev/ttyUSB1 monitor | tee /tmp/hq_led_lamp_esp.log
```

Read/grep the **actual** log:

```bash
grep -E "Firmware: title=" /tmp/hq_led_lamp_esp.log
grep -E "mqtt_cfg: verified transport configured" /tmp/hq_led_lamp_esp.log
grep -E "Verified TLS connected with validated identity" /tmp/hq_led_lamp_esp.log
grep -E "sync source=" /tmp/hq_led_lamp_esp.log
grep -E "sync-complete" /tmp/hq_led_lamp_esp.log
```

* Expected (assumption 2, §8): the lamp reconnects with its unchanged access token, no
  re-provisioning, no firmware change required; the same device entity (same `DEV_ID`) stays
  assigned to the Customer (`[PE docs]` — claiming does not change MQTT credentials; `[test]`).
* Tenant-side confirmation: `GET /api/tenant/devices?deviceName=$DEV_NAME` still resolves and the
  device id is unchanged; `connection_state` returns to `online` after the reboot
  (`[endpoint]`, `[test]`).
* Record the actual log lines; if the log shows re-provisioning or a token rejection, that is a
  PE/firmware interaction to report, not to assume away (`[test]`).

### 6.7 Customer User access: RPC / telemetry / attributes

As the Customer User (JWT), verify operational access after the claim (`[§9.5]`, `[test]`):

```bash
# Customer User login  [endpoint: POST /api/auth/login]
JWT="$(curl -sS -X POST "$TB_BASE_URL/api/auth/login" -H "Content-Type: application/json" \
  -d '{"username":"klc.test.a@example.com","password":"'$CUST_PASS'"}' | jq -r .token)"
cu() { curl -sS -H "Content-Type: application/json" -H "X-Authorization: Bearer $JWT" "$@"; }

# Two-way RPC as the Customer User  [endpoint: POST /api/rpc/twoway/{deviceId}]
cu -X POST "$TB_BASE_URL/api/rpc/twoway/$DEV_ID" \
  -d '{"method":"setState","params":{"power":true,"brightness":55},"timeout":10000}'

# Client attributes as the Customer User  [endpoint: GET .../values/attributes/CLIENT_SCOPE]
cu "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/attributes/CLIENT_SCOPE?keys=power,brightness"

# Latest telemetry as the Customer User  [endpoint: GET .../values/timeseries]
cu "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/timeseries?keys=power,brightness,connection_state"
```

* Expected: the RPC returns success and the lamp applies `power=true, brightness=55`; client
  attributes and telemetry are readable; the lamp publishes the new state as client attributes
  (`[manual §6]`, `[§9.5]`, `[test]`).
* Cross-check on the device log (monitor flow, §6.6): the applied state appears in the actual log
  (`[manual §7]`, `[test]`).
* Whether OTA/firmware updates are also visible to the Customer User on PE is assumption 3 (§8)
  (`[test]`).

### 6.8 Secret never appears in device or server logs

* Device log: grep the captured log for the secret value and (extra hygiene) for the MQTT access
  token value:

  ```bash
  # The claiming secret must never appear in the device log:
  grep -qF "$SECRET_VAL" /tmp/hq_led_lamp_esp.log && echo "SECRET LEAKED" || echo "secret not in device log"
  # The access token is not the claiming secret, but it must also not be logged
  # (device_identity "never logs the token" — [repo: main/app_main.c:37-40]).
  # TB_ACCESS_TOKEN = the device's MQTT access token, read by the operator from
  # the ThingsBoard device credentials (never from this document):
  grep -F "$TB_ACCESS_TOKEN" /tmp/hq_led_lamp_esp.log && echo "TOKEN LEAKED" || echo "token not in device log"
  ```

* Script output: `claim_handover.sh`/`claim_customer.sh` never print the secret (except the
  explicit `--handover` stdout channel) and redact it from error bodies (`[TASK-302]`, `[TASK-303]`).
  Capture the script stdout/stderr of steps 6.1–6.7 and grep them for the secret:
  `grep -F "$SECRET_VAL" <captured-script-output>` must find nothing (`[test]`).
* Server logs (PE host): search the ThingsBoard logs for the secret — exact command depends on the
  PE deployment:

  ```bash
  # Docker deployment:
  docker logs <thingsboard-pe-container> 2>&1 | grep -F "$SECRET_VAL" || echo "secret not in server logs"
  # Classic install:
  grep -rF "$SECRET_VAL" /var/log/thingsboard/ || echo "secret not in server logs"
  ```

  Expected: no match (`[test]`). The secret is stored only as the SERVER_SCOPE `claimingData`
  attribute and must not be written to logs by the backend/production process
  (`[manual §13.3]`, `[test]`).

### 6.9 Reclaim to the tenant (TASK-303 step)

```bash
scripts/thingsboard/claim_customer.sh "$DEV_NAME" reclaim
echo "exit=$?"   # expected 0
```

* Endpoint: `DELETE /api/customer/device/{deviceName}/claim` (`[endpoint]`, `[PE docs]`, `[§9.5]`).
* Expected: exit 0, `reclaim: device ... returned to the tenant`; the device is back under the
  tenant (tenant admin can see it again under `GET /api/tenant/devices`) (`[TASK-303]`, `[test]`).
* After reclaim, a further claim attempt with the old secret must fail (the secret was consumed;
  re-issuance = tenant overwrites `claimingData` with a new secret) (`[manual §13.3]`, `[test]`).

### 6.10 Lamp factory reset — identity behavior

With the device back under the tenant (or, as a variant, while still claimed — record both,
assumption 5, §8):

1. Hold the factory-reset button (default GPIO0 / BOOT, active-low) for 5 seconds
   (`[manual §12]`, `[repo: main/app_main.c factory_reset_*]`). The lamp blinks three times and
   restarts.
2. Capture the log (monitor flow) and grep the **actual** log:

   ```bash
   timeout 1m idf.py -p /dev/ttyUSB1 monitor | tee /tmp/hq_led_lamp_esp.log
   grep -E "Factory reset button held" /tmp/hq_led_lamp_esp.log
   grep -E "Factory reset completed; restarting" /tmp/hq_led_lamp_esp.log
   ```

3. Expected behavior (`[manual §12]`, `[repo]`): the lamp erases the Wi-Fi credentials and the
   runtime state under `/state`, then restarts into the Wi-Fi provisioning portal. The ThingsBoard
   identity record `/config/identity.json` (the access token) is **kept**
   (`[repo: components/device_identity/include/device_identity.h:107]`), so after new Wi-Fi
   credentials are entered via the portal the lamp reconnects as the **same** ThingsBoard device
   (same device id) — no re-provisioning, no new device entity (`[manual §12]`, `[test]`).
4. Confirm on PE: after factory reset + reconnect, the device entity is unchanged and its
   Customer/tenant assignment is the expected one (tenant after §6.9; if the variant was run while
   claimed, whether the factory reset changes the Customer assignment is assumption 5, §8)
   (`[test]`).

### 6.11 Cleanup (mandatory)

```bash
# Delete the test Customer User and Customer (tenant admin)  [endpoint: DELETE /api/user/{id},
#                                                            DELETE /api/customer/{id}]
tb -X DELETE "$TB_BASE_URL/api/user/$USER_ID" >/dev/null
tb -X DELETE "$TB_BASE_URL/api/customer/$CUST_ID" >/dev/null
# Remove claimingData if still present  [TASK-302]
scripts/thingsboard/claim_handover.sh "$DEV_NAME" --invalidate
# Delete the throwaway E2E device (optional, after all firmware checks)
tb -X DELETE "$TB_BASE_URL/api/device/$DEV_ID" >/dev/null
# Memory-only secrets die with the shell; never write them to files:
unset SECRET_VAL TB_CLAIM_SECRET TB_API_KEY JWT
```

## 7. Expected results summary

| # | Step | Actor / auth | Endpoint | Expected result | Exit code |
| --- | --- | --- | --- | --- | --- |
| 6.1 | Issue `claimingData` | Tenant admin (API key) | `POST .../attributes/SERVER_SCOPE` | issued, future expiration | 0 |
| 6.2 | Wrong claim | Customer User (JWT) | `POST /api/customer/device/{name}/claim` | rejected: wrong secret | 3 |
| 6.3 | Expired claim | Customer User (JWT) | `POST /api/customer/device/{name}/claim` | rejected: expired | 4 |
| 6.4 | Correct claim | Customer User (JWT) | `POST /api/customer/device/{name}/claim` | device transferred to Customer | 0 |
| 6.5 | Retry after claim | Customer User (JWT) | `POST /api/customer/device/{name}/claim` | rejected: already claimed | 5 |
| 6.6 | Reboot + MQTT | firmware + tenant | MQTT (unchanged token) | same device entity, online | — |
| 6.7 | RPC/telemetry/attributes | Customer User (JWT) | `POST /api/rpc/twoway/{id}`, telemetry/attribute GETs | success, state applied | — |
| 6.8 | Secret hygiene | device + server | — | secret absent from all logs | — |
| 6.9 | Reclaim | Customer User (JWT) | `DELETE /api/customer/device/{name}/claim` | device returns to tenant | 0 |
| 6.10 | Factory reset | firmware | — | identity kept, same device entity | — |

Exit codes 3–7 are the documented `claim_customer.sh` classification (`[TASK-303]`); on instances
that report generic rejections the reason cannot be told apart and exit 7 is used (`[TASK-303]`).

## 8. To be confirmed on PE

These are the unresolved assumptions 1–5 of the implementation plan. They are **not** proven by
this repository or by CE/dev-instance smoke tests; each is confirmed by the PE documentation and/or
the corresponding step of this scenario:

1. **Target version and `claimingData` format.** Assumed ThingsBoard PE ≥ 3.5/3.6. The
   `claimingData` format (`secretKey` + `expirationTime` in ms) and the
   `POST`/`DELETE /api/customer/device/{deviceName}/claim` endpoints were verified in code/tests on
   CE; on PE they must be confirmed against the documentation
   (https://thingsboard.io/docs/pe/user-guide/claiming/). Confirmed by: §6.1–§6.9.
2. **MQTT access token behavior after claim.** The PE documentation indicates that claiming does
   **not** change MQTT credentials (the device keeps connecting with the same token) — confirm on
   the target PE version. Confirmed by: §6.6 (reboot + continued MQTT, same device entity).
3. **Visibility of telemetry/attributes/RPC/OTA for the Customer User after claim** (PE vs CE
   permissions) — test on PE. Confirmed by: §6.7 (RPC/telemetry/attributes; OTA visibility).
4. **Claiming enablement at tenant/profile level.** Whether PE requires claiming to be enabled at
   the tenant/profile level — to be confirmed (PE documentation; §6.1/§6.4 fail with a clear
   "claiming disabled" style error if it is required).
5. **Reclaim and factory-reset interaction.** `DELETE .../claim` and its interaction with the
   lamp's factory reset — to be confirmed on PE. Confirmed by: §6.9 (reclaim) and §6.10 (factory
   reset identity behavior, including whether a factory reset while claimed changes the Customer
   assignment).

## 9. Evidence / artifacts

For the run to count as evidence on PE, collect:

* The captured device logs: `/tmp/hq_led_lamp_esp.log` (baseline §5, reboot §6.6, factory reset
  §6.10) and the actual grep output for each step — read/grep the log, do not assume behavior.
* The script stdout/stderr of §6.1–§6.9 (they must not contain the secret; §6.8).
* The ThingsBoard-side state at each step: device id before/after claim, Customer assignment,
  `connection_state`, client attributes, telemetry.
* The PE version/edition string and the claiming-related configuration (assumption 4, §8).
* Any deviation from the expected results in §7, recorded per step with the actual log/REST output.