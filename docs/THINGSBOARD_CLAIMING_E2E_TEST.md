# E2E: Claiming integration scenario on ThingsBoard PE

> **Status: NOT PASSED on PE — the PE requirement is NOT satisfied.** Reasons:
>
> 1. No **ThingsBoard PE** environment exists in this test environment: the only reachable
>    ThingsBoard is the development/test instance at `http://home-assistance.local:8080`
>    (confirmed **Community Edition 4.3.1.3**, not PE — `/api/license` returns HTTP 404
>    "No static resource api/license", and no PE image/container/instance is present on the
>    host). A **PE provisioning attempt** was made for the review round (§10.1, 2026-10-06):
>    the target PE image `thingsboard/tb-pe-node:4.3.1.6PE` (Professional Edition 4.3.1,
>    satisfies the ≥ 3.5/3.6 assumption) exists on Docker Hub and was pulled, but a run is
>    impossible without a PE license (`TB_LICENSE_SECRET`), which the license portal issues
>    only through an interactive signup (e-mail verification + Google reCAPTCHA) — not
>    completable from this environment. This task cannot be marked done on CE (Definition
>    of Done), so the target **PE** edition/version and the PE-specific assumptions remain
>    unverified.
> 2. A full **smoke run** of the TASK-304 scenario on the dev instance (CE 4.3.1.3) with the
>    **physical lamp** was executed and is recorded as facts in §10; the facts-vs-assumptions
>    analysis is in §11. Every CE result is a smoke-test result only and proves nothing about
>    PE (`[manual §13.6]`).
> 3. One scenario step could not be executed even as a smoke run: §6.10 (factory-reset button)
>    requires physically holding the GPIO0/BOOT button on the lamp for 5 s — no physical access
>    from this environment. The re-provisioning path was verified instead (see §11, assumption 5).
>
> The scenario below is the complete, runnable integration test for **ThingsBoard PE** with a
> **physical lamp**. It must be executed on an actual PE instance; the smoke run (§10) does not
> satisfy the PE requirement. Every ThingsBoard-dependent statement is tagged with
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

---

## 10. TASK-305 smoke-run record (development/test instance — **CE**, results are facts, not PE proof)

Run date: 2026-10-06. Instance: `http://home-assistance.local:8080` — **ThingsBoard Community
Edition 4.3.1.3** (`/api/license` → HTTP 404, no PE license endpoint; the only TB image on the
host is `thingsboard/tb-node:4.3.1.3`). Physical lamp: ESP32 `kitchen_led_controller` v0.1.1
(idf v5.5.5), MQTT over TLS `mqtts://home-assistance.local:8883`, client id `klc-kitchen-99`,
serial port `/dev/ttyUSB1`. This run does **not** satisfy the PE requirement of TASK-305; it is
the preparatory smoke run explicitly allowed by the task.

Secrets handling during the run: the claiming secret was generated and kept in memory only
(`--handover` stdout piped via `--secret-stdin`); it was never written to a file, to this
document, or to the device. The tenant API key was read at runtime from
`/home/dima/projects/hq_workspace/thingboard_tenant_api_key` and never printed or stored.

Captured artifacts (all under `/tmp`, outside the repository): baseline `/tmp/hq_led_lamp_esp.log`,
post-claim reboot `/tmp/hq_led_lamp_postclaim.log`, RPC-phase `/tmp/hq_led_lamp_rpc2.log`,
re-provisioning `/tmp/hq_led_lamp_reprovision.log`.

| Step | Actor / auth | Actual result (CE 4.3.1.3) | Exit / HTTP |
| --- | --- | --- | --- |
| §5 baseline | firmware + tenant | Log: `Firmware: title=kitchen_led_controller version=0.1.1`; `mqtt_cfg: verified transport configured: mqtts://... client_id=klc-kitchen-99`; `Verified TLS connected with validated identity`; `[tb_app] sync source=client`; `sync-complete(thingsboard)--> online (session 1)`. No Backtrace/Guru/errors. | — |
| §6.1 issue `claimingData` | tenant (API key) | `claimingData issued ... expires ... (600 seconds remaining)`; `--status` shows present + future expiration. Secret not printed (only `--handover` prints it). | 0 |
| §6.2 wrong claim | CU (JWT) | Rejected. Raw server response: HTTP 400, body `"FAILURE"` (no reason text on CE) → script exit **7** (not 3; the message-based classification of `[TASK-303]` cannot distinguish reasons on CE). Device owner unchanged (still tenant). | 7 |
| §6.3 expired claim | CU (JWT) | `claimingData` with `expirationTime` in the past; claim rejected: HTTP 400 body `"FAILURE"` → exit **7**. Check order (expiry vs secret) not observable on CE. Device owner unchanged. | 7 |
| §6.4 correct claim | CU (JWT) | Fresh 64-hex secret via `--handover` + `--secret-stdin` → `claim: device ... claimed by customer user 'klc.test.a@example.com'`. Owner changed to the test Customer; `claimingData` attribute consumed (reads as `[]`); device listed under the Customer's devices. | 0 |
| §6.5 retry after claim | CU (JWT) | Rejected: `claim rejected: device already claimed` — one-time use confirmed, secret consumed. | 5 |
| §6.6 reboot + MQTT | firmware + tenant | Post-claim monitor: same `client_id=klc-kitchen-99`, same TLS endpoint, `sync-complete --> online (session 1)`, **no re-provisioning**, no errors. Server: same device id `96a68b00` (unchanged), still assigned to the Customer, `connection_state=online`. | — |
| §6.7 RPC/telemetry/attributes | CU (JWT) | `POST /api/rpc/twoway/{id}` `setState {power:true, brightness:55}` → HTTP 200 `{"success":true,...,"applied":{...}}`. CLIENT_SCOPE attrs as CU: `power=true, brightness=55`. Telemetry as CU: `power=true, brightness=55, connection_state=online`. Device-log cross-check: `MQTT RX topic=v1/devices/me/rpc/request/1 payload={"method":"setState","params":{"power":true,"brightness":55}}`; state applied (`[tb_app] applied complete valid desired state: power=on brightness=55`). No firmware errors. | 200 |
| §6.8 secret hygiene | device + server | Device logs (all three captures): claiming secret **absent** (`grep -F`), no `credentialsId`/`credentialsValue`/`accessToken`/`Bearer` markers. Script `--status`/claim/reclaim outputs: secret **absent**. Server audit log via REST `/api/audit/logs`: **the claiming secret IS present in the `ATTRIBUTES_UPDATED` audit entries** (`actionData` contains the full `claimingData` value incl. `secretKey`) — see §11 finding 8. Raw server log files (docker logs / `/var/log/thingsboard`) could not be inspected (no shell access to the TB host). | — |
| §6.9 reclaim | CU (JWT) | `reclaim: device ... returned to the tenant`; owner back to the tenant. Old consumed secret after reclaim: rejected (exit 7 on CE). Re-issuance (new `claimingData` + claim): **exit 0** — re-issuance works. | 0 |
| Re-provisioning (credentials lost) | firmware + tenant | Server: device entity deleted (simulating lost credentials; the lamp was back under the tenant after §6.9). Device log: MQTT `CONNACK rejected ack=5` ×3 → `credentials rejected (1/3..3/3)` → `Re-provisioning scheduled after auth rejection` → `/provision/response` `{"credentialsValue":...,"credentialsType":"ACCESS_TOKEN","status":"SUCCESS"}` → `Device identity loaded` → TLS connect → `sync-complete --> online (session 4)`. Server: **new** device entity created (new id `1e1bf180…`, old `96a68b00…` deleted), owner = **tenant** (not a Customer), type `lamp_controller`, `connection_state=online`. | — |
| §6.10 factory reset | firmware | **Not executed** — requires physically holding the GPIO0/BOOT button for 5 s; no physical access from this environment. Identity-keeping behavior was not tested via the button path (see §11, assumption 5). | — |

### 10.1 PE provisioning attempt (TASK-305 continuation, review round, 2026-10-06)

Before re-recording the task status, a genuine attempt was made to obtain a **ThingsBoard PE**
environment for the required physical-lamp run. The findings are facts from this environment:

1. **No PE instance is present anywhere reachable.** A LAN scan (192.168.1.0/24, ports
   8080/8081/8082/8083/9090/1883/8883/7070) found exactly one ThingsBoard: the CE dev instance
   at `http://home-assistance.local:8080` (`192.168.1.2`; `/api/license` → HTTP 404 = no PE
   license endpoint). No other TB HTTP/MQTT port is open on the subnet.
2. **No shell access to the dev host.** `ssh dima@192.168.1.2` is refused (publickey), there is
   no Docker context for the remote host, and the only TB image on the local Docker host is the
   CE image `thingsboard/tb-node:4.3.1.3` — a PE container cannot be deployed on the host that
   runs the dev stack, and on this workstation a PE container would require a license that does
   not exist here (see point 4).
3. **The target PE image exists and its version was confirmed.** `docker pull
   thingsboard/tb-pe-node:4.3.1.6PE` (docker.io, amd64, published 2026-09-28, ~3.1 GB) — the
   current **Professional Edition** image, version **4.3.1.6** (tag `4.3.1.6PE`), matching the
   "PE ≥ 3.5/3.6" target assumption 1 of the implementation plan. This is the edition/version
   the scenario would be executed against; it is **not** a confirmation that PE behavior was
   exercised.
4. **The blocker is the license, verified from the image itself.** PE refuses to run without a
   license: `conf/thingsboard.conf` requires `TB_LICENSE_SECRET` ("License secret obtained from
   ThingsBoard License Portal (https://license.thingsboard.io)"), and
   `conf/thingsboard.yml` binds `license.secret: "${TB_LICENSE_SECRET:}"` (no default, no
   offline/trial fallback in the image). The license portal issues trial/commercial licenses
   only through an interactive signup (client API: `/api/noauth/signup/recaptchaParams`,
   `/api/noauth/signup`, `/api/signup/*`, all behind Google reCAPTCHA v2/v3 + e-mail
   verification). This environment has no e-mail inbox and no way to solve the reCAPTCHA, so
   the 30-day trial cannot be obtained automatically. No license material (secret/instance
   data file) exists anywhere in the workspace.
5. **Consequence.** The physical-lamp run on PE (§6.1–§6.10) cannot be executed from this
   environment. The CE smoke run (§10) remains the only physical-lamp evidence and proves
   nothing about PE. **TASK-305 stays open** until the environment owner provides a reachable
   ThingsBoard PE instance with a valid license (and, if it is a new instance, the lamp's
   provisioning credentials for its tenant, cf. §3 prerequisites).

## 11. TASK-305 conclusions: facts vs assumptions

Legend: ✔=confirmed by the smoke run (CE only, not PE proof), ✘=refuted by the smoke run (CE),
⛔=not testable in this environment / PE unverified.

1. **`claimingData` format and endpoints (assumption 1; PE unverified).** ✔ on CE 4.3.1.3:
   `POST /api/plugins/telemetry/DEVICE/{id}/attributes/SERVER_SCOPE` with
   `{"claimingData":"{\"secretKey\":...,\"expirationTime\":<ms>}"}` works; the claim endpoint
   `POST /api/customer/device/{name}/claim` and `DELETE .../claim` work exactly as documented
   (steps §6.1–§6.9). ⛔ On PE: no PE instance available — version/format remain unverified.
2. **MQTT access token behavior after claim (assumption 2; PE unverified).** ✔ on CE: the lamp
   rebooted after the claim and reconnected with the **same** access token and **same device
   entity** (same client id, same device id, no re-provisioning — §6.6). This matches the PE
   documentation claim that claiming does not change MQTT credentials. ⛔ On PE: unverified.
3. **Customer User visibility of telemetry/attributes/RPC (assumption 3; PE unverified).** ✔ on CE:
   after the claim the Customer User could read client attributes and telemetry and execute
   two-way RPC (`setState` applied by the lamp, confirmed both via HTTP response and via the
   device log — §6.7). OTA visibility as CU was not tested. ⛔ On PE: unverified (CE CU
   permissions are what was exercised).
4. **Claiming enablement at tenant/profile level (assumption 4).** ✔ on CE: no explicit
   claiming-related enablement was present or needed — the claim worked out of the box. ⛔ On PE:
   unverified.
5. **Reclaim and factory-reset interaction (assumption 5).** ✔ reclaim works (device returns to
   the tenant; the old consumed secret stops working; re-issuance = overwriting `claimingData`
   with a new secret works — §6.9). ⛔ Factory-reset-button behavior (identity kept, same entity)
   was **not** executed — no physical access to the GPIO0 button. The firmware's identity-keeping
   + automatic re-enrollment path was verified via the credentials-lost run instead (see 7).
6. **Post-claim lifecycle (manual §13.6 assumption 6; PE unverified).** ✔ on CE: (a) reboot after
   claim → same entity, still assigned to the Customer, no firmware action needed; (b) lost
   credentials (device deleted server-side) after reclaim → automatic re-provisioning creates a
   **new tenant-owned** device entity with a new access token — ownership silently moves to a new
   entity exactly as it would for a never-claimed device. The "re-provision **while still
   claimed**" variant was **not** run (the lamp was reclaimed first); whether re-provisioning
   while claimed preserves Customer assignment is ⛔ unverified and remains a PE check.
7. **Firmware behavior (required by TASK-305: evidence from actual logs).** All conclusions above
   are based on the captured monitor logs (`/tmp/hq_led_lamp_esp.log`,
   `/tmp/hq_led_lamp_postclaim.log`, `/tmp/hq_led_lamp_rpc2.log`,
   `/tmp/hq_led_lamp_reprovision.log`) and REST state: connectivity, telemetry, RPC handling, the
   3×-rejection re-enrollment path and the `/provision/response` exchange were read from the logs,
   not assumed. No Backtrace, Guru Meditation, or unexpected errors appeared in any capture.
8. **FINDING — the claiming secret DOES appear in the ThingsBoard audit log (refutes the strict
   "never in server logs" reading).** Writing `claimingData` via the REST API creates an
   `ATTRIBUTES_UPDATED` audit entry whose `actionData` contains the full attribute value,
   including `secretKey`. On the dev instance this was verified by grepping the REST audit log
   (`/api/audit/logs/entity/DEVICE/{id}`) for the memory-only secret: the `ATTRIBUTES_UPDATED`
   entries contain it (masked in §10's evidence). Consequences for the production process:
   - the secret is intentionally absent from **device logs** (✔ verified) and from the script
     outputs (`claim_handover.sh`/`claim_customer.sh`, ✔ verified),
   - but anyone with audit-log read access (tenant admin, sysadmin, and on some
     editions/configurations customer-scoped audits) can recover the secret from the server-side
     audit trail while it is valid, so the audit log must be treated as a secret-bearing store:
     access-controlled, not shipped off-platform, and `claimingData` should be invalidated
     (claimed/expired/re-issued) as soon as the handover completes,
   - the "never in logs" statement in `[manual §13.3]` is therefore only true for device logs,
     script output, and this documentation — the ThingsBoard audit log stores the attribute value
     by design. The manual is updated in §13.3 to state this explicitly.
9. **Wrong-claim error classification differs on CE vs the documented exit codes.** On the dev
   instance every rejected claim returns HTTP 400 with body `"FAILURE"` (no reason text), so
   `claim_customer.sh` reports exit **7** (generic) for wrong/expired secrets and only
   **"already claimed"** (exit 5) and **"claimed"** (exit 0) are distinguishable by the server
   response. On PE the message-based classification (exit 3/4/6) must be re-verified because it
   depends on the server's error-message text.
10. **PE provisioning is blocked by licensing (2026-10-06, review round).** Facts: the only
    ThingBoard reachable from this environment is CE 4.3.1.3 (LAN scan + `/api/license` 404);
    the target PE image `thingsboard/tb-pe-node:4.3.1.6PE` (Professional Edition 4.3.1.6) was
    pulled and its license requirement (`TB_LICENSE_SECRET`, no default/fallback) was verified
    from the image's own `conf/`; the license portal issues licenses only via interactive
    signup (e-mail + reCAPTCHA), which this environment cannot complete (§10.1). ⛔ Therefore
    assumptions 1–5 stay PE-unverified, the physical-lamp PE run (§6.1–§6.10) stays unexecuted,
    and **TASK-305 remains open**; the DoD (PE results incl. confirmed PE version/edition of a
    *running* PE instance) is not met and cannot be met on CE alone.
