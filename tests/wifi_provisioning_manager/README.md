# wifi_provisioning_manager — on-target verification and manual procedure (TASK-128)

The provisioning portal is inherently an **on-target, radio-level** feature:
a phone or laptop joins the device's temporary access point, is captured by
the captive DNS responder, and submits the home SSID/password through the
HTTP portal.  Host mocks (`wifi_provisioning_mock.c` and friends, TASK-126)
cannot verify this end to end; the unit tests cover the adapter contract and
the credentials-never-logged rule, while this document is the on-target
counterpart:

- the **manual on-target procedure** that reproducibly provisions a fresh
  device,
- the **automated monitor-log smoke check** wired into the standard
  flash/monitor loop (`on_target_smoke_check.py`),
- the **expected log signatures** for every provisioning transition
  (portal startup, portal stop, provisioning success, provisioning
  failure/park),
- the **failure-mode matrix** with the state each failure leaves the device
  in and how it recovers.

## Context: the provisioning stage in the boot flow

The boot order contract (see `main/app_main.c` file header, TASK-127) places
Wi-Fi provisioning between the Wi-Fi gate and TLS:

```
boot -> safe-off -> filesystem -> configuration -> Wi-Fi
     -> provisioning (ONLY when no saved station credential exists)
     -> verified MQTT/TLS -> state sync -> online
```

`main/app_main.c` owns the decision through the adapter's
`wifi_provisioning_manager_has_saved_credentials()` query.  A fresh device
(no saved credential) enters `PROVISIONING`; a credentialed device passes
the network gate straight to TLS and never enters the portal.

Device-side portal facts used by the procedure (platform defaults):

| Fact                    | Value                                              |
|-------------------------|----------------------------------------------------|
| Provisioning AP SSID    | `Bimbrownik:<MAC>` (e.g. `Bimbrownik:a0:b1:c2:d3:e4:f5`) |
| Provisioning AP password| `SuperTrudne1!-_`                                  |
| Portal HTTP listen URL  | `http://0.0.0.0:80` (reachable at `http://10.10.0.1`) |
| Captive DNS listen URL  | `udp://0.0.0.0:53` (captures every DNS query and redirects to the portal) |
| Portal API              | `POST /api/v1/wifi/credentials` (JSON `{"ssid":…,"password":…}`), `GET /api/v1/wifi/status`, `GET /api/v1/wifi/networks` |
| Success grace interval  | `CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS`, default `2000` ms in this product's `sdkconfig.defaults` (0 retires immediately, max 60000; single grace source of truth since TASK-131 — the duplicate `CONFIG_KLC_PROVISIONING_GRACE_MS` was dropped) |
| Provisioning wait window| `PROVISIONING_WAIT_TIMEOUT_MS` = `180000` ms (bounded station connect window) |
| Network gate timeout    | `NETWORK_CONNECT_TIMEOUT_MS` = `30000` ms          |

---

## Manual on-target procedure

Goal: take a **fresh** device (no saved station credential), boot it,
provision it through the portal with a test SSID/password, and confirm the
portal retires after the grace interval and the device proceeds to the TLS
gate.

### 0. Prerequisites

- An ESP32-WROOM-32D on a serial port (this workspace: `/dev/ttyUSB1`).
- The ESP-IDF v5.5.5 environment (`source …/esp-idf/export.sh`; the on-target
  record below was captured on v5.5.5, see the boot-log `ESP-IDF: v5.5.5`
  line).
- A phone or laptop with Wi-Fi (for the interactive steps below; the
  automated smoke check itself needs no second radio).
- A lab / throwaway **home** WLAN whose SSID and password you control —
  the device must be able to *connect* to it for the success path.

### 1. Prepare the device: configuration payload on storage

A **fresh device** (no `wifi_ap.json`, never provisioned) must still pass
the earlier gates of the boot order contract — filesystem **and**
configuration — before the network gate can decide to enter provisioning.
The configuration gate validates the four product documents and the trust
anchor on the LittleFS `storage` partition:

| Storage path            | Document                                    |
|-------------------------|---------------------------------------------|
| `/config/device.json`   | `{"schema_version":1,"product":…,"hardware_revision":…,"serial":…,"thingsboard_name":…}` |
| `/config/manufacturing.json` | `{"schema_version":1,"manufacturing_state":1,"credential_mode":1}` (provisioned + PSK) |
| `/config/mqtt.json`     | broker/TLS document (schema v1, `tls_mode:"mqtts"`, `ca_path:"/cert/ca.crt"`) |
| `/config/identity.json` | `{"schema_version":1,"client_id":…,"access_token":…}` (labtoken; never logged) |
| `/cert/ca.crt`          | CA that verifies the MQTT endpoint (`server/certs/ca.crt` in the dev LAN stack) |

`idf.py flash` never touches the `storage` partition (OTA-style flash
preserves user data), so on a never-flashed or `erase-flash`'d device the
documents must be written once with a pre-built LittleFS image.  Use the
platform's `littlefs_util` host tool (geometry must match the ESP backend:
**4096-byte blocks**, `0x60000` = 393216 bytes = 96 blocks for the
`storage` partition):

```sh
# 1a. Assemble the payload (one-time; keep manufacturing_state=1,
#     credential_mode=1 so device_identity_load() accepts the record).
mkdir -p /tmp/klc_payload/config /tmp/klc_payload/cert
cp server/certs/ca.crt /tmp/klc_payload/cert/ca.crt
# … create /tmp/klc_payload/config/{device.json,manufacturing.json,
#   mqtt.json,identity.json} per the table above …

# 1b. Build the storage image (needs gcc; the tool's own build issue is
#     avoided by compiling with the C99 standard the platform uses).
cd platform/hq_platform/tools
gcc -std=gnu99 -o /tmp/littlefs_util littlefs_util.c \
    ../third_party/littlefs/lfs.c ../third_party/littlefs/lfs_util.c \
    ../third_party/littlefs/bd/lfs_filebd.c \
    -I ../third_party/littlefs -I ../third_party/littlefs/bd
/tmp/littlefs_util --create /tmp/klc_payload \
    --out /tmp/klc_storage.littlefs --size 393216

# 1c. Fresh flash: erase the whole flash (fresh NVS -> no saved station
#     credential; fresh storage), write the storage image at 0x3A0000,
#     then flash the firmware (flash does not touch storage).
cd /home/dima/projects/hq_workspace/hq_led_lamp
source /home/dima/projects/esp-idf/export.sh
idf.py erase-flash -p /dev/ttyUSB1
python -m esptool --chip esp32 -p /dev/ttyUSB1 -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 4MB --flash_freq 40m \
    0x3A0000 /tmp/klc_storage.littlefs
idf.py flash -p /dev/ttyUSB1
```

> **Configuration gate note:** with the documents absent the boot parks at
> `configuration --config-fail(configuration)--> safe-off` and never
> reaches the provisioning stage — the automated smoker would see no
> provisioning transitions.  The configured fresh device above is the
> reproducible starting point the smoke check assumes.
>
> **Main-task stack note (on-target finding, TASK-128):** the configured
> boot first loads and *applies* the broker/TLS document on the main task
> (`mqtt_cfg_apply()` keeps three ~5 KiB certificate snapshots on the
> stack).  With the ESP-IDF default stack (3584 B) the device aborts with
> `***ERROR*** A stack overflow in task main` — a `Backtrace:` line the
> smoker flags.  `sdkconfig.defaults` therefore sets
> `CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768`; without it a configured fresh
> device cannot boot backtrace-free.

### 2. Boot and confirm the provisioning stage entry

Capture the boot log in a monitor session:

```sh
timeout 1m idf.py -p /dev/ttyUSB1 monitor | tee /tmp/hq_led_lamp_esp.log
```

Within the first seconds of boot the fresh device must show the portal
startup signatures (full wording in the [log signatures](#expected-log-signatures)
section):

```
… klc: No saved station credential; entering Wi-Fi provisioning (portal)
[INFO]: [prov_mgr] provisioning portal running (listeners bound)
… klc: Provisioning portal running; waiting for the station to submit a credential and connect …
```

The device is now broadcasting `Bimbrownik:<MAC>` on `10.10.0.1/24`.

### 3. Join the provisioning AP

On the phone/laptop, join the temporary AP:

- **SSID:** `Bimbrownik:<MAC>` (see the device's actual MAC in the last six
  AP SSID octets; the boot log does not print the AP name — read the
  `Bimbrownik:` network from the Wi-Fi scan list).
- **Password:** `SuperTrudne1!-_`

### 4. Submit the test SSID/password through the captive portal

Open any URL in the phone/laptop browser — the captive DNS responder answers
every A record with the portal address, so any address lands on
`http://10.10.0.1`.  In the portal page:

1. Trigger/confirm a scan (`POST /api/v1/wifi/scans`, `GET /api/v1/wifi/networks`).
2. Select (or type) the lab WLAN SSID and enter its password.
3. Submit (`POST /api/v1/wifi/credentials`).

Use the **documented smoke-test credential** when possible so the automated
no-secret-leak assertion of the smoke check (assertion (c) below) mirrors
exactly what was submitted:

- test SSID: `KLC-Smoke-128-Test`
- test password: `KLC-Sm0ke-Pa55-128!`

(If the lab WLAN has different values, that is fine — pass the same values
to the smoke check via `--test-ssid` / `--test-password`.)

### 5. Confirm the grace retire and the TLS gate

Watch the monitor session.  Once the device connects to the lab WLAN the
portal stays up for `CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS`
(default 2 s in `sdkconfig.defaults`), then
retires and the boot chain continues:

```
[INFO]: [prov_mgr] provisioning portal stopped
… klc: Provisioning succeeded and portal retired; continuing to the NETWORK gate -> TLS
… klc: Network connected; verified MQTT/TLS connect is now allowed …
… klc: Verified TLS connected with validated identity; …
```

The `Bimbrownik:<MAC>` network disappears from the Wi-Fi scan list (the AP
is retired).  The device now has a saved credential: a reflash without an
`erase-flash` boots straight through the network gate to TLS
(pass-through; the portal never starts — verify with the smoke check in
`--expect pass-through` mode).

**Reproducibility check:** repeat steps 1–5 on a second fresh device (or
after `erase-flash`) and confirm the same signature sequence appears.

---

## Automated on-target smoke check

`on_target_smoke_check.py` adds provisioning assertions to the standard
flash/monitor loop.  Given a captured monitor log it asserts:

- **(a)** the log is backtrace-free (no `Backtrace:` line),
- **(b)** the provisioning state transitions observed in the log match the
  legal state-machine sequence (see the automaton in the script; it mirrors
  `app_state.h` — `NETWORK --PROVISIONING_STARTED--> PROVISIONING
  --PROVISIONING_SUCCEEDED/FAILED--> NETWORK | SAFE_OFF`, with the
  bounded-retry re-entry after a provisioning failure being legal),
- **(c)** no SSID or password substring of the test credential appears
  anywhere in the captured log (secrecy rule, TASK-120/126).

It prints the observed transition sequence (useful for task notes) and
exits non-zero on the first failed assertion.

### Fresh-device run (the standard flash/test loop)

```sh
cd /home/dima/projects/hq_workspace/hq_led_lamp
source /home/dima/projects/esp-idf/export.sh

# Prepare the configured fresh device (step 1): erase + storage image
# with the config documents at 0x3A0000 + app flash.  A bare
# erase-flash + flash leaves the configuration gate FAILING (no
# device.json) and no provisioning transition ever appears.
idf.py erase-flash -p /dev/ttyUSB1
python -m esptool --chip esp32 -p /dev/ttyUSB1 -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 4MB --flash_freq 40m \
    0x3A0000 /tmp/klc_storage.littlefs
idf.py flash -p /dev/ttyUSB1

timeout 1m idf.py -p /dev/ttyUSB1 monitor | tee /tmp/hq_led_lamp_esp.log
grep -q 'Backtrace:' /tmp/hq_led_lamp_esp.log && { echo "Backtrace found!"; exit 1; } || true

python3 tests/wifi_provisioning_manager/on_target_smoke_check.py \
    --log /tmp/hq_led_lamp_esp.log \
    --expect provisioning
```

Within the 1-minute window the fresh device shows
`ENTER -> RUN` (portal entered and running; the wait window is 180 s, so the
outcome is not expected inside the standard window).  The check accepts the
legal *prefix*.

The script can also run the loop itself (app flash + bounded monitor;
the configured-fresh-device preparation from step 1 is a prerequisite):

```sh
python3 tests/wifi_provisioning_manager/on_target_smoke_check.py \
    --port /dev/ttyUSB1 --monitor-duration 60 --expect provisioning
```

### Credentialed-device (pass-through) run

After a successful manual provision, a reflash (no erase) must skip the
portal:

```sh
idf.py flash -p /dev/ttyUSB1
timeout 1m idf.py -p /dev/ttyUSB1 monitor | tee /tmp/hq_led_lamp_esp.log
python3 tests/wifi_provisioning_manager/on_target_smoke_check.py \
    --log /tmp/hq_led_lamp_esp.log --expect pass-through
```

### Options / environment

| Option | Default | Meaning |
|--------|---------|---------|
| `--log FILE` | – | validate an already captured log (`-` = stdin); skip flash/monitor |
| `--port` | `/dev/ttyUSB1` | serial port for the flash/monitor loop |
| `--monitor-duration` | `60` | bounded monitor window in seconds |
| `--expect` | `provisioning` | `provisioning` (fresh: must enter the portal), `pass-through` (credentialed: must skip it), `any` |
| `--test-ssid` | `KLC-Smoke-128-Test` | test SSID whose substrings must not appear in the log (`$KLC_SMOKE_TEST_SSID`) |
| `--test-password` | `KLC-Sm0ke-Pa55-128!` | test password whose substrings must not appear (`$KLC_SMOKE_TEST_PASSWORD`) |

`--test-ssid`/`--test-password` default to the documented smoke-test
credential; pass the actual lab values so the secrecy assertion mirrors the
real provision.

---

## Expected log signatures

All signatures are **credential-free** by construction (TASK-120/126): a
transition message may contain state names, durations, error codes or
statuses — never an SSID, a password, a token or a URL with embedded
credentials.  `klc:` lines are `ESP_LOG` from `main/app_main.c`; `[prov_mgr]`
lines are the adapter's OSAL logs (printed raw to the UART).

### Portal startup

| Log line | Meaning |
|----------|---------|
| `… klc: No saved station credential; entering Wi-Fi provisioning (portal)` | NETWORK gate observed no saved credential; `PROVISIONING_STARTED` delivered (NETWORK → PROVISIONING) |
| `[INFO]: [prov_mgr] provisioning portal running (listeners bound)` | Adapter `start()` succeeded: HTTP portal + captive DNS listeners bound |
| `… klc: Provisioning portal running; waiting for the station to submit a credential and connect (bounded wait, watchdog fed)` | Supervisor entered the bounded station wait |

### Portal stop

| Log line | Meaning |
|----------|---------|
| `[INFO]: [prov_mgr] provisioning portal stopped` | Adapter `stop()` closed the portal listeners (every stop; the shared Mongoose process/MQTT/TLS untouched) |
| `… klc: Provisioning succeeded and portal retired; continuing to the NETWORK gate -> TLS` | Success path: grace honored, portal retired, `PROVISIONING_SUCCEEDED` delivered (PROVISIONING → NETWORK) |
| `… klc: No station connection within 180000 ms; stopping the portal and parking degraded` | Failure path: wait window elapsed, portal being stopped, `PROVISIONING_FAILED` pending |

### Provisioning success

The full success signature is (the adapter logs the portal stop *before*
the supervisor logs the success line — `stop_provisioning_if_active()`
runs first, then `PROVISIONING_SUCCEEDED` is logged):

```
[INFO]: [prov_mgr] provisioning portal stopped
… klc: Provisioning succeeded and portal retired; continuing to the NETWORK gate -> TLS
… klc: Network connected; verified MQTT/TLS connect is now allowed …
… klc: Verified TLS connected with validated identity; …
```

`Network connected` is the NETWORK gate passing after the provision
(saved credential present); `Verified TLS connected with validated identity`
is the TLS gate.  If the lab WLAN cannot reach the ThingsBoard broker the
TLS line is replaced by
`… klc: Verified TLS connect failed: … (output forced off, …)` — the
provisioning stage itself still succeeded (this is a post-provisioning
network/broker condition, not a provisioning failure).

### Provisioning failure / park

| Log line | Meaning |
|----------|---------|
| `… klc: Provisioning portal start failed; machine parks degraded (bounded retry)` (plus `[ERROR]: [prov_mgr] provisioning portal start failed (platform_state=%d)`) | Portal start refused (bind/Mongoose); `PROVISIONING_FAILED` delivered |
| `… klc: No station connection within 180000 ms; stopping the portal and parking degraded` | No station connected inside the wait window (no client, or a wrong credential); `PROVISIONING_FAILED` delivered |
| `[INFO]: [prov_mgr] provisioning portal stopped` | Portal listeners closed on the failure path |
| `… klc: Safe-off (degraded): no retry scheduled; waiting for provisioning / reset / OTA` | Final park (non-retryable) |
| `… klc: Safe-off (degraded): retry budget exhausted; waiting for provisioning / reset / OTA` | Park after the bounded retry budget (5 attempts) is spent |

A provisioning failure parks the machine **degraded** (SAFE_OFF) with the
existing bounded retry/backoff — never a portal restart storm.  The retry
returns to the NETWORK stage, which re-enters provisioning only while no
saved credential exists (`ENTER -> RUN -> …` again in the same log).

---

## Failure-mode matrix

Every row lists the trigger, the observable log signature, the state the
device ends in, and the recovery path.

### 1. Portal bind failure

| | |
|---|---|
| **Trigger** | `wifi_provisioning_manager_start()` refuses: shared Mongoose process not running, or an HTTP/DNS listener cannot bind (port taken, resource exhaustion). |
| **Log** | `… klc: Provisioning portal start failed; machine parks degraded (bounded retry)` and `[ERROR]: [prov_mgr] provisioning portal start failed (platform_state=%d)`; no `portal running` line. |
| **End state** | `SAFE_OFF` (degraded); `PROVISIONING_FAILED` scheduled a bounded retry to NETWORK. |
| **Recovery** | The bounded retry re-attempts the portal; once the binding condition clears it succeeds. If the retry budget exhausts, the device parks (no storm) and waits for provisioning / reset / OTA. |
| **Verification** | Host unit tests (`wifi_provisioning_mock` failure injection + Mongoose-not-running precondition); on-target signature as above. |

### 2. No client within the wait window

| | |
|---|---|
| **Trigger** | The portal runs `PROVISIONING_WAIT_TIMEOUT_MS` (180 s) and no station ever connects (nobody joined the AP / nobody submitted). |
| **Log** | `… klc: No station connection within 180000 ms; stopping the portal and parking degraded`, `[INFO]: [prov_mgr] provisioning portal stopped`, then the park/retry lines of the failure signature. |
| **End state** | `SAFE_OFF` (degraded) with a bounded retry; the retry re-enters NETWORK → PROVISIONING (fresh device), so the portal comes back up after the backoff and the user gets another window. |
| **Recovery** | Join `Bimbrownik:<MAC>` and submit a valid credential while the next window is open, or reset the device. |
| **Verification** | **On target (TASK-128 task notes):** configured fresh device left untouched >180 s → `No station connection within 180000 ms; …`, `[prov_mgr] provisioning portal stopped`, `provisioning --provisioning-failed--> safe-off`, then `safe-off --retry-due--> network` re-enters the portal (`ENTER -> RUN` repeats).  Smoke check accepts `ENTER -> RUN -> WAIT_TIMEOUT -> PORTAL_STOPPED -> ENTER -> RUN`. |

### 3. Wrong submitted credential

| | |
|---|---|
| **Trigger** | A client submits an SSID/password the device cannot use (typo, wrong key, out-of-range AP).  The device switches to STA mode, the connect attempt fails, and `network_manager_is_connected()` never turns true. |
| **Log** | Identical product signature to row 2 — the device **never learns or logs the submitted value** (secrecy rule), so it only observes "no station connection within the window", then `… klc: No station connection within 180000 ms; … parking degraded` + `[prov_mgr] provisioning portal stopped`. |
| **End state** | `SAFE_OFF` (degraded) with bounded retry, same as row 2. |
| **Recovery** | Rejoin the (re-started) AP and submit the correct credential, or reset. |
| **Verification** | The device-observable half is the same on-target run as row 2 (the wait window expires with no connection; no submitted value is ever learned or logged — secrecy rule).  On target: join the AP, submit a deliberately wrong credential, watch the window expire into the timeout/park signature.  The portal-side "connect failed" status is visible to the client in `GET /api/v1/wifi/status`, never in the device log. |

### 4. AP retirement race (success path race / grace expiry)

| | |
|---|---|
| **Trigger** | Station connects, the success-grace interval (`CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS`, default 2 s in `sdkconfig.defaults`) elapses and the portal retires while a client is still attached or a repeat submission races the teardown. |
| **Log** | `[INFO]: [prov_mgr] provisioning portal stopped`, `… klc: Provisioning succeeded and portal retired; continuing to the NETWORK gate -> TLS`, then the NETWORK/TLS gate lines. |
| **End state** | `NETWORK` → `TLS` — the credential is saved, so the retry/`DISCONNECTED` path reconnects through the network gate with the saved credential; the machine never re-enters provisioning. |
| **Recovery** | None required.  If the station drop happens during teardown, the network gate's bounded retry reconnects with the saved credential.  A client submitting twice inside the grace window gets a retry/idempotent response — the portal's second `stop()` is a safe no-op. |
| **Verification** | **Pending: requires a lab WLAN** — the interactive success path (join AP → submit → grace retire → NETWORK/TLS handoff) was **not** exercised on target during this milestone (no lab WLAN available; see the task notes).  The success signature set and the automaton are documented above and asserted mechanically by the smoke check whenever the path is exercised; teardown idempotency (double stop) is covered by the adapter host unit tests. |

---

## What the host unit tests cover (vs. on target)

| Concern | Where it is verified |
|---------|----------------------|
| Adapter lifecycle, start/stop idempotency, failure propagation, URL overrides, concurrency | Host unit tests (`wifi_provisioning_manager_test.c`, `ctest`) |
| Credentials-never-logged secrecy rule | Host unit tests (log-content regression) **and** on-target smoke check assertion (c) |
| Portal startup/stop observable in the log | On target (smoke check assertion (b) + manual procedure) |
| End-to-end provision (radio: join AP → submit → connect → grace retire) | On target manual procedure (needs a second radio and a lab WLAN) |

---

## Task notes (TASK-128 verification record)

Observed with `/dev/ttyUSB1` (ESP32-WROOM-32D), ESP-IDF v5.5.5
(the boot log reports `ESP-IDF: v5.5.5`):

1. **On-target finding — main-task stack.**  A configured fresh device
   (config documents on storage) aborted at the FIRST configuration
   application with `***ERROR*** A stack overflow in task main` +
   `Backtrace:` while the ESP-IDF default `CONFIG_ESP_MAIN_TASK_STACK_SIZE`
   (3584 B) was active — the standard flash/monitor no-backtrace assertion
   caught it immediately.  Root cause: `mqtt_cfg_apply()` captures the
   applied CA / mTLS material as three `mqtt_cfg_cert_snapshot_t` stack
   locals (~5 KiB each = ~15.4 KiB in one frame).  Fixed by setting
   `CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768` in `sdkconfig.defaults` (16 KiB
   still overflowed).  After the fix the configured fresh boot is
   backtrace-free end to end.
2. **Fresh configured device run** (`erase-flash` → storage image with
   `device.json`, `manufacturing.json` (provisioned+PSK), `mqtt.json`,
   `identity.json`, `ca.crt` at `0x3A0000` → `idf.py flash` →
   `timeout 1m idf.py … monitor`):

   ```
   I klc: Filesystem ready: storage mounted at /littlefs …
   [app_state] filesystem --fs-ok(filesystem)--> configuration
   I klc: device.json loaded: product='Kitchen LED Controller' …
   I klc: manufacturing.json loaded: state=1 mode=1
   I klc: Device identity loaded (access-token auth, …)
   [app_state] configuration --config-ok(configuration)--> network
   I klc: No saved station credential; entering Wi-Fi provisioning (portal)
   [app_state] network --provisioning-started(network)--> provisioning
   [INFO]: [prov_mgr] provisioning portal running (listeners bound)
   I klc: Provisioning portal running; waiting for the station to submit a
          credential and connect (bounded wait, watchdog fed)
   ```

   Smoke check `--expect provisioning` **PASSES** with the legal sequence
   `ENTER -> RUN`, no `Backtrace:`, and no
   `KLC-Smoke-128-Test` / `KLC-Sm0ke-Pa55-128!` substring.

   **Re-verified on `/dev/ttyUSB1`** (this milestone, final review pass):
   `idf.py flash -p /dev/ttyUSB1` + `timeout 1m idf.py -p /dev/ttyUSB1
   monitor | tee /tmp/hq_led_lamp_esp.log` reproduced the identical entry
   boot (configuration gate → `No saved station credential; entering
   Wi-Fi provisioning (portal)` → `[prov_mgr] provisioning portal running
   (listeners bound)` → supervisor wait line), zero `Backtrace:`, and the
   smoke check PASSED with `ENTER -> RUN`.
3. **Failure path (rows 2/3) on target** — extended monitor (>180 s) with
   no client, then a bounded retry re-entry:

   ```
   W klc: No station connection within 180000 ms; stopping the portal and
          parking degraded
   [INFO]: [prov_mgr] provisioning portal stopped
   [app_state] provisioning --provisioning-failed(network)--> safe-off
   [app_state] safe-off --retry-due(timer)--> network
   I klc: No saved station credential; entering Wi-Fi provisioning (portal)
   [app_state] network --provisioning-started(network)--> provisioning
   [INFO]: [prov_mgr] provisioning portal running (listeners bound)
   ```

   Smoke check **PASSES** with the legal sequence
   `ENTER -> RUN -> WAIT_TIMEOUT -> PORTAL_STOPPED -> ENTER -> RUN`
   (the `safe-off` park is silent here because a retry is pending — the
   `Safe-off (degraded): …` lines only fire for the non-retryable and
   retry-budget-exhausted parks, both seen earlier in the milestone).
4. Interactive success path (join `Bimbrownik:<MAC>`, submit credentials in
   the captive portal, watch the grace retire and the NETWORK→TLS handoff)
   requires a lab WLAN reachable from the device; the signature set is
   documented above and each signature is asserted mechanically by the
   smoke checker on the entry path (and by the automaton on the success
   path when exercised).

Run the smoke check against a captured log at any time:

```sh
python3 tests/wifi_provisioning_manager/on_target_smoke_check.py \
    --log /tmp/hq_led_lamp_esp.log --expect any
```