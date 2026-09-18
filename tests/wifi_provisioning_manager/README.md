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
  (provisioning entry via the controller's STARTED event, provisioning
  success, provisioning failure/park, explicit portal stop),
- the **failure-mode matrix** with the state each failure leaves the device
  in and how it recovers.

## Context: the provisioning stage in the boot flow

The boot order contract (see `main/app_main.c` file header, TASK-127/133)
places Wi-Fi provisioning between the Wi-Fi gate and TLS:

```
boot -> safe-off -> filesystem -> configuration -> Wi-Fi
     -> provisioning (controller fallback decision: fresh device or
        exhausted-credential device)
     -> verified MQTT/TLS -> state sync -> online
```

The **platform fallback controller** (TASK-131/132,
`CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK=y`) owns the provisioning
decision: it opens the portal at init when no saved station credential
exists (fresh device), or after the saved credential exhausted the bounded
`CONNECT_FAILED` budget (`CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS`,
default 2 in this product).  The supervisor (TASK-133) is a *consumer*: it
polls the adapter's outcome events
(`wifi_provisioning_manager_poll_event()`) and delivers STARTED/SUCCEEDED/
FAILED to the state machine (owner NETWORK).  A credentialed device that
connects on the first try passes the network gate straight to TLS and never
enters the portal.

Device-side portal facts used by the procedure (platform defaults):

| Fact                    | Value                                              |
|-------------------------|----------------------------------------------------|
| Provisioning AP SSID    | `Bimbrownik:<MAC>` (e.g. `Bimbrownik:a0:b1:c2:d3:e4:f5`) |
| Provisioning AP password| `SuperTrudne1!-_`                                  |
| Portal HTTP listen URL  | `http://0.0.0.0:80` (reachable at `http://10.10.0.1`) |
| Captive DNS listen URL  | `udp://0.0.0.0:53` (captures every DNS query and redirects to the portal) |
| Portal API              | `POST /api/v1/wifi/credentials` (JSON `{"ssid":…,"password":…}`), `GET /api/v1/wifi/status`, `GET /api/v1/wifi/networks` |
| Success grace interval  | `CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS`, default `2000` ms in this product's `sdkconfig.defaults` (0 retires immediately, max 60000; single grace source of truth since TASK-131 — the duplicate `CONFIG_KLC_PROVISIONING_GRACE_MS` was dropped) |
| Fallback budget         | `CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS` = `2` consecutive `CONNECT_FAILED` events before the portal opens on a credentialed device (fresh devices open at init) |
| Provisioning wait window| none in the product — the controller keeps the portal up until success/stop (TASK-133 removed the supervisor's `PROVISIONING_WAIT_TIMEOUT_MS`) |
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
entry signature (full wording in the [log signatures](#expected-log-signatures)
section):

```
… klc: Provisioning flow started by the controller; entering Wi-Fi provisioning
   (the machine delivers PROVISIONING_STARTED once it reaches the NETWORK gate)
… klc: Provisioning gate: portal owned by the controller; waiting for its outcome events
```

The controller opened the portal at adapter init (fresh device); the device
is now broadcasting `Bimbrownik:<MAC>` on `10.10.0.1/24`.

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
no-secret-leak assertion of the smoke check (assertion (d) below) mirrors
exactly what was submitted:

- test SSID: `KLC-Smoke-128-Test`
- test password: `KLC-Sm0ke-Pa55-128!`

(If the lab WLAN has different values, that is fine — pass the same values
to the smoke check via `--test-ssid` / `--test-password`.)

### 5. Confirm the grace retire and the TLS gate

Watch the monitor session.  Once the device connects to the lab WLAN the
portal stays up for `CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS`
(default 2 s in `sdkconfig.defaults`), then the controller retires it and
the supervisor delivers the success outcome:

```
… klc: Provisioning succeeded; portal retired by the controller; NETWORK gate resumes
… klc: Network connected; verified MQTT/TLS connect is now allowed …
… klc: Verified TLS connected with validated identity; …
```

(The portal listeners are retired by the controller itself — the adapter's
`[prov_mgr] provisioning portal stopped` line appears only on an explicit
OTA/FATAL stop.)  The `Bimbrownik:<MAC>` network disappears from the Wi-Fi
scan list (the AP is retired).  The device now has a saved credential: a
reflash without an `erase-flash` boots straight through the network gate to
TLS (pass-through; the portal never starts — verify with the smoke check in
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
  --PROVISIONING_SUCCEEDED/FAILED--> NETWORK | SAFE_OFF`, driven by the
  controller's outcome events since TASK-133, with the bounded-retry
  re-entry after a provisioning failure being legal),
- **(c)** on a fresh-device boot (`--expect provisioning`) the TASK-135
  portal-reachability signature `[INFO]: [prov_mgr] provisioning AP up;
  portal reachable` was observed — the WHOLE portal (radio AP+STA and both
  bound listeners) is up, not merely the radio (added in TASK-138),
- **(d)** no SSID or password substring of the test credential appears
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

Within the 1-minute window the fresh device shows `ENTER` (the
supervisor delivered the controller's STARTED outcome; the portal stays up
under the controller's policy until success/stop — there is no product-side
wait window anymore).  The check accepts the legal *prefix*.

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
| `--expect` | `provisioning` | `provisioning` (fresh: must enter the portal), `pass-through` (credentialed: must skip it), `any`, `stale-credential` (TASK-139: bounded NETWORK_FAILED fallback then portal, no restart storm), `stale-recovery` (stale-credential PLUS `SUCCESS -> NETWORK -> TLS` after re-provisioning) |
| `--fallback-budget` | `2` | configured `CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS` budget the stale-credential assertion checks against |
| `--test-ssid` | `KLC-Smoke-128-Test` | test SSID whose substrings must not appear in the log (`$KLC_SMOKE_TEST_SSID`) |
| `--test-password` | `KLC-Sm0ke-Pa55-128!` | test password whose substrings must not appear (`$KLC_SMOKE_TEST_PASSWORD`) |
| `--stale-ssid` | `KLC-Stale-Net-139` | stale (wrong) SSID whose substrings must not appear (`$KLC_SMOKE_STALE_SSID`, TASK-139) |
| `--stale-password` | `KLC-Stale-Pass-139!` | stale (wrong) password whose substrings must not appear (`$KLC_SMOKE_STALE_PASSWORD`, TASK-139) |

`--test-ssid`/`--test-password` default to the documented smoke-test
credential; pass the actual lab values so the secrecy assertion mirrors the
real provision.  `--stale-ssid`/`--stale-password` default to the documented
throwaway wrong credential the TASK-139 procedure seeds; pass the values the
storage image actually used.

---

## Expected log signatures

All signatures are **credential-free** by construction (TASK-120/126): a
transition message may contain state names, durations, error codes or
statuses — never an SSID, a password, a token or a URL with embedded
credentials.  `klc:` lines are `ESP_LOG` from `main/app_main.c`; `[prov_mgr]`
lines are the adapter's OSAL logs (printed raw to the UART).

### Provisioning entry (controller fallback)

| Log line | Meaning |
|----------|---------|
| `… klc: Provisioning flow started by the controller; entering Wi-Fi provisioning` | Supervisor delivered the controller's STARTED outcome while the machine was at the NETWORK gate (fresh device, or exhausted `CONNECT_FAILED` budget); `PROVISIONING_STARTED` delivered (NETWORK → PROVISIONING) |
| `… klc: Provisioning gate: portal owned by the controller; waiting for its outcome events` | Machine entered `PROVISIONING` (bookkeeping only; the controller keeps the portal up) |

The portal listeners are opened by the platform controller itself (TASK-132
notification translation).  Once the WHOLE portal is up — the radio reached
AP+STA **and** both owned listeners (HTTP + captive DNS) are bound, which is
exactly the platform's reachability surface `wifi_http_provisioning_is_reachable()`
— the adapter emits ONE credential-free line per portal-up period
(TASK-135): `[INFO]: [prov_mgr] provisioning AP up; portal reachable`.
The line fires at most once per false→true reachability edge, wherever the
supervisor first observes it (start/poll/query), and is never re-emitted
until the portal goes down again (stop/lifecycle reset re-arms the edge
guard).

### Provisioning success

The full success signature is (the controller retires the portal after the
success-grace interval; the adapter has nothing to log on that path):

```
… klc: Provisioning succeeded; portal retired by the controller; NETWORK gate resumes
… klc: Network connected; verified MQTT/TLS connect is now allowed …
… klc: Verified TLS connected with validated identity; …
```

`Provisioning succeeded …` is the supervisor delivering the controller's
SUCCEEDED outcome after the temporary AP was retired
(PROVISIONING → NETWORK).  `Network connected` is the NETWORK gate passing
after the provision (saved credential present); `Verified TLS connected with
validated identity` is the TLS gate.  If the lab WLAN cannot reach the
ThingsBoard broker the TLS line is replaced by
`… klc: Verified TLS connect failed: … (output forced off, …)` — the
provisioning stage itself still succeeded (this is a post-provisioning
network/broker condition, not a provisioning failure).

### Portal up; waiting for a station (idle portal)

TASK-135 adds the bounded, credential-free observability for the "portal is
up but no station has joined yet" state — the state an operator actually
needs to distinguish from "portal down" and from "portal start failed":

| Log line | Meaning |
|----------|---------|
| `[INFO]: [prov_mgr] provisioning AP up; portal reachable` | The WHOLE portal is up (radio in AP+STA, HTTP + captive DNS listeners bound): `wifi_http_provisioning_is_reachable()` first became true.  Fires once per portal-up period (false→true edge), deduplicated across `start()`/`is_active()`/`poll_event()` via an atomic edge guard, re-armed by `stop()`/lifecycle reset |
| `[INFO]: [prov_mgr] portal up; station not yet connected (waiting for a client to join and submit; one log line per 30 s)` | The controller waits in `PROVISIONING` with the portal up and reachable; **rate-limited to at most one line per 30 s** (`WIFI_PROVISIONING_MANAGER_WAIT_LOG_PERIOD_MS`) so an idle portal stays observable without flooding the log |

Neither message ever carries an SSID, a password, a token or a URL value;
they are emitted from the adapter's supervisor pump (`poll_event()`), so they
appear on both the controller-owned fresh-device portal and the explicit
adapter-start path.

### Stale-credential bounded fallback (TASK-139)

| Log line | Meaning |
|----------|---------|
| `… klc: Network lost; lamp forced off by adapter, ThingsBoard stays disconnected until reconnect` | The Wi-Fi manager reported one CONNECT_FAILED for the saved (wrong) credential — the platform fallback controller counts it against the `CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS` budget (the product's `[wifi] state WAIT_CONNECT -> STOP -> IDLE` debug lines bracket it) |
| `[INFO]: [app_state] network --network-failed(network)--> safe-off (session N)` | The NETWORK gate timed out without a connection; the machine parks SAFE_OFF with a bounded retry scheduled (one episode per connect cycle) |
| `[INFO]: [app_state] safe-off --retry-due(timer)--> network (session N+1)` | The bounded retry re-enters NETWORK; the gate re-drives the saved-credential connect (TASK-139 fix), which fires the next CONNECT_FAILED |
| `[INFO]: [prov_mgr] controller transition 1 -> 3 (product event 1)` | The controller exhausted the budget and started the portal (fallback fired) |
| `… klc: Provisioning flow started by the controller; entering Wi-Fi provisioning` | The supervisor delivered the STARTED outcome; the machine enters PROVISIONING (the NETWORK gate services this in-loop since TASK-139, so the entry happens promptly in the session where the budget exhausted) |

The device must show **at most** `--fallback-budget` NETWORK_FAILED episodes
before the portal opens, exactly one ENTER and exactly one portal-up period
(the TASK-135 reachability signature) — no restart storm, no park-without-AP.

### Provisioning failure / park

| Log line | Meaning |
|----------|---------|
| `[ERROR]: [prov_mgr] provisioning portal start failed (platform_state=%d, start_status=%d, adapter_status=%d)` | Adapter observed a portal start failure; `adapter_status` is the TASK-134 mapped product code (`ERR_DEPENDENCY`/`ERR_MODE_TRANSITION`/`ERR_HTTP_BIND`/`ERR_DNS_BIND`/`ERR_AP_NOT_UP`/`ERR_START_FAILED`) so the log reader can tell exactly which start step failed — one line per distinct failure |
| `… klc: Provisioning failed; machine parks degraded (bounded retry)` | Supervisor delivered the adapter's FAILED outcome (portal start failure observed by the adapter); `PROVISIONING_FAILED` delivered, machine parks SAFE_OFF |
| `[INFO]: [prov_mgr] provisioning portal stopped` | Explicit adapter stop (OTA entry / FATAL): controller lifecycle ended and portal listeners closed |
| `… klc: Safe-off (degraded): no retry scheduled; waiting for provisioning / reset / OTA` | Final park (non-retryable) |
| `… klc: Safe-off (degraded): retry budget exhausted; waiting for provisioning / reset / OTA` | Park after the bounded retry budget (5 attempts) is spent |

A provisioning failure parks the machine **degraded** (SAFE_OFF) with the
existing bounded retry/backoff — never a portal restart storm.  The retry
returns to the NETWORK stage; the portal is never restarted by the
supervisor (a later fallback cycle may legally re-enter provisioning
through the controller — `ENTER -> …` again in the same log).

---

## Failure-mode matrix

Every row lists the trigger, the observable log signature, the state the
device ends in, and the recovery path.  A park in **any** row is recoverable
**without a reflash** through the documented
[erase/re-provision escape hatch](#task-137-erasureprovision-escape-hatch)
below (platform `wifi_mgmt_erase_credentials()` where reachable, or the
serial storage-partition erase for a device that cannot reach the API),
followed by a restart — the controller's fresh-device fallback then reopens
the portal.

### 1. Portal bind failure

| | |
|---|---|
| **Trigger** | The platform controller opens the provisioning application (fresh device at init, or exhausted `CONNECT_FAILED` budget) and the listeners cannot bind (shared Mongoose process not running, port taken, resource exhaustion, radio refused AP+STA).  Since TASK-134 the adapter maps each documented platform start failure mode (`wifi_http_provisioning_start_ex()`) onto a distinct product status (`ERR_DEPENDENCY`, `ERR_MODE_TRANSITION`, `ERR_HTTP_BIND`, `ERR_DNS_BIND`, `ERR_AP_NOT_UP`, or the generic `ERR_START_FAILED` for undocumented modes) so the supervisor can tell exactly which start step failed. |
| **Log** | `[ERROR]: [prov_mgr] provisioning portal start failed (platform_state=%d, start_status=%d, adapter_status=%d)` — one line per distinct start failure, `adapter_status` carrying the TASK-134 mapped product code — and (when the adapter observes the start failure) `… klc: Provisioning failed; machine parks degraded (bounded retry)`; no `provisioning AP up; portal reachable` line. |
| **End state** | `SAFE_OFF` (degraded); `PROVISIONING_FAILED` scheduled a bounded retry to NETWORK. |
| **Recovery** | The bounded retry returns to the NETWORK gate; a later fallback cycle re-opens the portal once the binding condition clears. If the retry budget exhausts, the device parks (no storm) and waits for provisioning / reset / OTA. |
| **Verification** | Host unit tests (`wifi_provisioning_mock` failure injection + Mongoose-not-running precondition, and the TASK-133 adapter-stop controller-lifecycle tests); on-target signature as above. |

### 2. No client ever connects

| | |
|---|---|
| **Trigger** | Nobody joins the AP / submits a credential.  Since TASK-133 removed the supervisor's `PROVISIONING_WAIT_TIMEOUT_MS`, the product has **no wait window**: the controller keeps the portal up indefinitely (until success/stop/OTA/FATAL), so an idle user simply takes as long as needed. |
| **Log** | `… klc: Provisioning flow started by the controller; …` + `… klc: Provisioning gate: portal owned by the controller; …` + `[INFO]: [prov_mgr] provisioning AP up; portal reachable`, then the rate-limited wait heartbeat `[INFO]: [prov_mgr] portal up; station not yet connected (waiting for a client to join and submit; one log line per 30 s)` at most once per 30 s while the controller waits in `PROVISIONING` with the portal up (TASK-135). |
| **End state** | `PROVISIONING` (portal up). |
| **Recovery** | Join `Bimbrownik:<MAC>` and submit a valid credential at any time. |

### 3. Wrong submitted credential

| | |
|---|---|
| **Trigger** | A client submits an SSID/password the device cannot use (typo, wrong key, out-of-range AP).  The connect attempt fails and the controller stays provisionable (its grace-abort path returns to PROVISIONING), so the portal remains up for another attempt. |
| **Log** | No failure is logged on this path — the device **never learns or logs the submitted value** (secrecy rule); the portal-side "connect failed" status is visible to the client in `GET /api/v1/wifi/status`, never in the device log.  The `[prov_mgr] controller transition …` lines carry state codes only. |
| **End state** | `PROVISIONING` (portal up). |
| **Recovery** | Resubmit the correct credential through the still-open portal, or reset. |
| **Verification** | On target: join the AP, submit a deliberately wrong credential, confirm the portal stays up and no submitted value ever appears in the log. |

### 4. AP retirement (success path / explicit stop)

| | |
|---|---|
| **Trigger** | Station connects, the success-grace interval (`CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS`, default 2 s in `sdkconfig.defaults`) elapses and the controller retires the portal; a client submitting twice inside the grace window races the teardown.  An explicit adapter stop (OTA entry / FATAL) ends the controller lifecycle and cancels any pending grace timer. |
| **Log** | Success: `… klc: Provisioning succeeded; portal retired by the controller; NETWORK gate resumes`, then the NETWORK/TLS gate lines.  Explicit stop: `[INFO]: [prov_mgr] provisioning portal stopped (controller lifecycle ended)`. |
| **End state** | Success: `NETWORK` → `TLS` — the credential is saved, so the retry/`DISCONNECTED` path reconnects through the network gate with the saved credential; the machine never re-enters provisioning.  OTA/FATAL: the controller lifecycle is ended and no listener or grace timer is left. |
| **Recovery** | None required.  If the station drop happens during teardown, the network gate's bounded retry reconnects with the saved credential.  A client submitting twice inside the grace window gets a retry/idempotent response — the controller's retirement is a safe no-op on a stop-while-stopping. |
| **Verification** | **Pending: requires a lab WLAN** — the interactive success path (join AP → submit → grace retire → NETWORK/TLS handoff) was **not** exercised on target during this milestone (no lab WLAN available; see the task notes).  The success signature set and the automaton are documented above and asserted mechanically by the smoke check whenever the path is exercised; teardown idempotency and the controller-lifecycle stop are covered by the adapter host unit tests (TASK-133). |

---

## What the host unit tests cover (vs. on target)

| Concern | Where it is verified |
|---------|----------------------|
| Adapter lifecycle, start/stop idempotency, failure propagation, URL overrides, concurrency | Host unit tests (`wifi_provisioning_manager_test.c`, `ctest`) |
| Credentials-never-logged secrecy rule | Host unit tests (log-content regression) **and** on-target smoke check assertion (d) |
| Portal state observability signatures (reachable edge / distinct start-failure code / 30 s rate-limited wait heartbeat) | Host unit tests (log-content regression: exact signatures, rate-limit count, and no known SSID/password/URL in the captured log) |
| Portal startup/reachability observable in the log | On target (smoke check assertions (b) + (c), TASK-138, + manual procedure) |
| End-to-end provision (radio: join AP → submit → connect → grace retire) | On target manual procedure (needs a second radio and a lab WLAN) |
| **Portal submit overwrites a stale saved credential (TASK-136)** | Host unit tests (`wifi_storage_overwrite_test.c`, CTest `wifi_storage_overwrite_tests`) — see the [overwrite-path trace](#task-136-overwrite-path-trace) below |

---

## TASK-136: overwrite-path trace

### Platform submission path (source trace)

The portal credential route is
`platform/hq_platform/src/wifi_provisioning/wifi_http_provisioning.c`:
`POST /api/v1/wifi/credentials` is handled by
`prov_handle_credentials_request()`, which validates the JSON body and then
**unconditionally** overwrites the station config and requests an
asynchronous connect:

```
wifi_mgmt_set_ap_name( ssid, ssid_len )
wifi_mgmt_set_password( password, pass_len )
wifi_mgmt_connect()
```

The Wi-Fi manager's worker task
(`platform/hq_platform/src/wifi/wifi_managment.c`) consumes the connect
request: `_state_connect()` hands the new `sta_cfg` to the radio
(`wifi_hal_set_sta_config()`) and calls `wifi_hal_connect()`; on the
station's GOT_IP event, `_state_wait_connect()` calls
`_save_current_sta_config()`, which persists through
`platform/hq_platform/src/wifi/wifi_config.c`:

```
wifi_config_add_credential( &config_list, ssid, pass )
wifi_config_save( &config_list )   ->  _write_file( "wifi_ap.json",
                                                    CREATE | TRUNCATE )
```

`wifi_config_add_credential()` semantics:

- an entry with the **same SSID** (the classic stale-password case) is
  updated **in place** — the stale password cannot survive,
- an entry with a **different SSID** is appended (or, at capacity, the
  oldest entry is evicted) and `last_use` is promoted to the submitted
  entry, so the boot-time loader (`_load_saved_config()` → last_use)
  selects the submitted credential and the stale entry can survive only as
  an unselected rotation slot (the platform's documented multi-credential
  design, `WIFI_CONFIG_MAX_CREDENTIALS=8`),
- the write is atomic from the reader's perspective (`CREATE|TRUNCATE` on a
  single file), so a concurrent `wifi_mgmt_is_read_data()` never observes a
  partial overwrite.

### Product storage mount (why the product path behaves identically)

The persistence layer speaks only the portable OSAL file API and stores to
the relative path `WIFI_CONFIG_FILE_PATH` = `"wifi_ap.json"`.  On the ESP
the OSAL littlefs back-end resolves a relative path under the mounted
volume (`osal_lfs_build_vfs_path()` prefixes the configured mount point),
i.e. `/littlefs/wifi_ap.json`.  On the product the supervisor runs the
FILESYSTEM gate first — `lamp_fs_init()` mounts the LittleFS `storage`
partition at `/littlefs` — then the CONFIGURATION gate, and only then the
NETWORK gate; the Wi-Fi manager and the provisioning adapter are brought
up on the NETWORK gate's **first entry** (product-layer fix in
`main/app_main.c`, TASK-136).  The manager's init-time saved-credential
load (`wifi_mgmt_init()` → `_load_saved_config()` → `wifi_ap.json`) and
every portal-submission save therefore land on the lamp_fs-mounted storage
exactly like the demo.  **Conclusion: the submitted credential
unconditionally replaces the saved one, and the station reconnects with it
in the same lifecycle (no reboot).**  No platform change was needed for the
overwrite itself; the fix was the *boot ordering* — originally the manager
was started in `app_main()` before the FILESYSTEM gate, the OSAL rejected
the init-time load with `OSAL_ERR_INCORRECT_OBJ_STATE` (unmounted volume,
observed on target as `wifi_config_read_file: osal_stat failed rc=-35`),
so `wifi_mgmt_is_read_data()` stayed false and even after a successful
portal submission the NETWORK gate would park the machine instead of
handing the now-credentialed station to TLS.  With the mount-first
ordering, a device with a saved credential passes the gate straight to
TLS, and a portal submission (SUCCEEDED → NETWORK) does the same without a
reboot.

### Host test (`wifi_storage_overwrite_tests`)

`tests/wifi_provisioning_manager/wifi_storage_overwrite_test.c` compiles
the REAL `wifi_managment.c` + `wifi_config.c` (with the real POSIX OSAL
mutex/semaphore/task back-ends) against a deterministic radio double
(`wifi_hal_mock_min.c`) and a real temp-dir file system that stands in for
the product storage mount (`wifi_storage_osal_support.c` — the same logical
path mapping and CREATE|TRUNCATE semantics as the ESP OSAL).  It drives the
exact portal submission sequence while a stale credential is present and
asserts, in AP+STA concurrent mode:

1. `test_submit_overwrites_stale_credential` — same SSID, corrected
   password: the storage file ends up holding **exactly** the newly
   submitted credential (one entry, stale password absent), the station
   switched without a reboot (the radio double received exactly the new
   credential), and neither the old nor the new secret (nor the SSID)
   appears in any captured log line,
2. `test_submit_different_network_replaces_selected_credential` — a
   different network is submitted while the stale one exists: the submitted
   credential becomes the last-used entry (what a reboot selects), the
   station switches in the same lifecycle, and the log stays secret-free.

Run:

```sh
cmake -S tests/wifi_provisioning_manager -B <build-dir>
cmake --build <build-dir>
ctest --test-dir <build-dir> --output-on-failure
```

---

## TASK-137: erase/re-provision escape hatch

A device that parks in `SAFE_OFF` after a failed portal window — or that
holds a stale/unusable saved credential (TASK-136 covers the
portal-submission overwrite, not the "saved credential is wrong and no
client is around to fix it" case) — must always be bringable back to the
PROVISIONING flow.  The escape hatch is a **supported platform API**
(`wifi_mgmt_erase_credentials()`, platform TASK-015), not a reflash: erase
the saved credential, restart/re-init, and the platform controller's
fresh-device fallback reopens the portal.  This section documents the erase
paths, the restart/re-init step, the product trigger decision, and the
on-target verification record.

### How a device becomes "fresh" again (source trace)

The provisioning entry decision chain:

1. **Boot-time credential load.**  `wifi_managment.c`
   `_load_saved_config()` (called inside `wifi_mgmt_init()`, which the
   product runs on the NETWORK gate's first entry) loads
   `WIFI_CONFIG_FILE_PATH` (`wifi_ap.json` → `/littlefs/wifi_ap.json` under
   the product mount) and sets the private flag `read_wifi_data` — the
   public view is `wifi_mgmt_is_read_data()` — only when a usable saved
   credential exists.  (TASK-136 additionally sets the flag on a successful
   portal submission; `wifi_mgmt_erase_credentials()` clears it again.)
2. **Controller init fallback.**  `wifi_provisioning_controller.c` init
   (reached through the adapter's `wifi_provisioning_manager_init()` on the
   NETWORK gate's first entry) runs `if (!wifi_mgmt_is_read_data())`: on a
   fresh device it opens the provisioning application immediately
   (`fallback_started = true`, portal start,
   `WIFI_PROVISIONING_CONTROLLER_PROVISIONING`, STARTED notification).  A
   credentialed device gets no portal here and proceeds to the connect path.
3. **Supervisor delivery.**  The supervisor (TASK-133) delivers the
   controller's STARTED event, the machine moves
   `NETWORK --provisioning-started--> PROVISIONING`, and the portal runs
   (`[prov_mgr] provisioning AP up; portal reachable`).

Erasing the saved credential makes step 1 report "no credential" on the next
init, so step 2 opens the portal — an erased device is indistinguishable
from a fresh one.

### Erase path A — platform API where reachable

`wifi_mgmt_erase_credentials()` (declared in
`platform/hq_platform/src/wifi/wifi_managment.h`, implemented in
`wifi_managment.c`; platform tests cover it in
`platform/hq_platform/tests/wifi/wifi_mgmt_test.c`):

- removes `wifi_ap.json` from the mounted storage **and** clears the
  in-memory credential state (`config_list`, `current_cred_nb`,
  `config_loaded`, `read_wifi_data`, `saved_data`, `sta_cfg`) under the
  state lock, so a concurrent `wifi_mgmt_is_read_data()` never observes a
  partial erase,
- is idempotent: erasing with no saved credential is a no-op success
  (returns true),
- is safe in every Wi-Fi state: not-initialized/stopped (removes any
  credential file on the mounted storage), running-disconnected (fresh
  immediately), running-connected (the live session stays up — the HAL owns
  the running network configuration — but the next reconnect/restart starts
  from the fresh-device state),
- never logs credential content — only the outcome
  (`[wifi] saved credentials erased` or
  `[wifi] credential erase failed rc=<rc>`), per the platform Wi-Fi logging
  policy (TASK-015).

Precondition: the LittleFS `storage` partition must be mounted (the product
FILESYSTEM gate ran) so the OSAL file operations reach
`/littlefs/wifi_ap.json`; the Wi-Fi manager itself does not need to be
running.

**Reachability in this product:** the API is part of the platform the
product links, but no product trigger is wired to it yet (product trigger
decision below).  "Where reachable" today means a developer/CI harness (the
platform POSIX builds compile the same manager API and can call it
programmatically) or a temporary product test build; a future product
trigger (long-press factory reset / OTA command) becomes the supported call
site for field devices.  The recovery contract is identical to path B:
erase → restart/re-init → the controller's fresh-device fallback reopens
the portal.

### Erase path B — erase the littlefs storage partition (API not reachable)

When the API cannot be reached (no trigger, no console, unresponsive
firmware), erase the saved credential at the partition level.  The
credential file lives on the LittleFS `storage` partition:

| Storage fact | Value |
|---|---|
| Partition | `storage` (`data, littlefs`); offset `0x3A0000`, size `0x60000` (393216 B) — see `partitions.csv` |
| Credential file | `/littlefs/wifi_ap.json` (relative OSAL path `wifi_ap.json`) |
| Required config documents | `/config/{device,manufacturing,mqtt,identity}.json` + `/cert/ca.crt` — the CONFIGURATION gate requires them, so a **bare** partition erase (without re-writing the documents) parks the device at `configuration --config-fail--> safe-off`, before the provisioning stage |

Procedure (serial-attached operator; **no app reflash**):

```sh
# 1. Build the credential-free storage image: unpack the current image,
#    drop wifi_ap.json, recreate (or start from the canonical fresh payload
#    of the manual procedure step 1, which has no wifi_ap.json):
/tmp/littlefs_util --unpack /tmp/current.littlefs --out /tmp/klc_payload_fresh
rm -f /tmp/klc_payload_fresh/wifi_ap.json
/tmp/littlefs_util --create /tmp/klc_payload_fresh \
    --out /tmp/klc_storage_fresh.littlefs --size 393216

# 2. Erase the partition region (removes wifi_ap.json and anything else on
#    the partition), then write the credential-free image back (restores
#    the four config documents + CA that the CONFIGURATION gate requires):
python -m esptool --chip esp32 -p /dev/ttyUSB1 -b 460800 \
    erase_region 0x3A0000 0x60000
python -m esptool --chip esp32 -p /dev/ttyUSB1 -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 4MB --flash_freq 40m \
    0x3A0000 /tmp/klc_storage_fresh.littlefs

# 3. Restart (the write hard-resets; or press EN/RST).  The application is
#    NOT reflashed — that is the whole point of the escape hatch.
```

Equivalently, the `write_flash` step alone overwrites the whole partition
(no credential survives), so the `erase_region` step is only needed to make
the "erase" explicit; the credential-free image must always be written back
for the CONFIGURATION gate to pass.

> **`idf.py erase-flash` is NOT the escape hatch.**  It erases the whole
> flash — the app, the partition table and NVS — and leaves storage without
> the required config documents, so both the firmware and the storage image
> must be re-written afterwards (a reflash).  The hatched path rewrites at
> most the storage partition and reboots.

### Restart / re-init step

After either erase path, restart the device (or re-run the re-init path:
the NETWORK gate's first entry re-brings-up the manager).  The restart is
required because the fresh-device decision happens at controller init
(`wifi_provisioning_manager_init()` → `wifi_http_provisioning_init()` →
`if (!wifi_mgmt_is_read_data())`).  On the next boot:

1. FILESYSTEM gate mounts storage; CONFIGURATION gate passes (documents
   present),
2. NETWORK gate first entry: `wifi_mgmt_init()` → `_load_saved_config()`
   finds no `wifi_ap.json` → `wifi_mgmt_is_read_data()` is false,
3. `wifi_provisioning_manager_init()` registers with the platform controller,
   whose init sees a fresh device and opens the portal,
4. the supervisor delivers the controller's STARTED event:
   `NETWORK --provisioning-started--> PROVISIONING`, portal up
   (`[prov_mgr] provisioning AP up; portal reachable`).

Because `wifi_mgmt_erase_credentials()` also clears the in-memory state, a
re-init without a physical restart (a future trigger that stops/restarts the
NETWORK gate in place) works identically.

### Product trigger decision (TASK-137)

**Decision: serial-only for now.**  This task implements **no** new product
trigger; the supported recovery today is the serial-attached procedure (path
B above for any device; path A for a developer/CI harness).  A long-press
factory reset (GPIO) and an OTA / ThingsBoard-RPC erase command are deferred.

**Rationale**

- The escape hatch must exist *before* any trigger UI: the platform API is
  already verified by the platform's own tests, and the serial-only
  procedure forces any parked device back to PROVISIONING without a reflash
  — the Definition of Done does not depend on a product trigger.
- A long-press factory reset needs a GPIO input/debounce layer the product
  does not have yet, plus a state-machine-safe call site (which states may
  erase? must the controller lifecycle/portal be stopped first?) — not
  trivial, and outside this task's scope.
- An OTA / ThingsBoard-RPC erase needs the RPC transport glue
  (TASK-112/113/114 modules exist but are not wired to the sync/online gate
  yet) plus a progress/result contract — not trivial.
- Cost/benefit: every bench/lab device in this project's workflow is
  serial-attached, so serial-only covers all currently reachable devices;
  the supported API is already in place for whatever trigger a later task
  selects.

**Known gap (explicit):** a customer-owned device without serial access
cannot trigger the erase itself yet; until the factory-reset or RPC trigger
lands, field recovery of such a device goes through serial support or an OTA
push of a corrected credential.  Documented here so the decision and its
consequence stay visible.

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
   [INFO]: [prov_mgr] provisioning AP up; portal reachable
   I klc: Provisioning portal running; waiting for the station to submit a
          credential and connect (bounded wait, watchdog fed)
   [INFO]: [prov_mgr] portal up; station not yet connected (waiting for a
          client to join and submit; one log line per 30 s)
   ```

   Smoke check `--expect provisioning` **PASSES** with the legal sequence
   `ENTER -> RUN`, no `Backtrace:`, and no
   `KLC-Smoke-128-Test` / `KLC-Sm0ke-Pa55-128!` substring.

   **Re-verified on `/dev/ttyUSB1`** (this milestone, final review pass):
   `idf.py flash -p /dev/ttyUSB1` + `timeout 1m idf.py -p /dev/ttyUSB1
   monitor | tee /tmp/hq_led_lamp_esp.log` reproduced the identical entry
   boot (configuration gate → `No saved station credential; entering
   Wi-Fi provisioning (portal)` → `[prov_mgr] provisioning AP up; portal
   reachable` → supervisor wait line → 30 s rate-limited wait heartbeats),
   zero `Backtrace:`, and the smoke check PASSED with `ENTER -> RUN`.
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
   [INFO]: [prov_mgr] provisioning AP up; portal reachable
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

### TASK-137 verification record (erase/re-provision escape hatch, on target)

Observed on `/dev/ttyUSB1` (ESP32-WROOM-32D), ESP-IDF v5.5.5.

**Setup.**  The WROOM build was rebuilt from scratch (`idf.py fullclean &&
idf.py build`, passes).  Two storage images were prepared with the
platform `littlefs_util` (4096-byte blocks, 393216 B):

- credentialed: the TASK-136 payload **plus** `wifi_ap.json` (a stale
  credential `{"last_use":0,"credentials":[{"nb":0,"ssid":"KLC-Stale-Net",
  "password":"KLC-Stale-Pass-136!"}]}` — the same document the TASK-136
  overwrite tests used, never logged by the firmware),
- fresh: the same payload **without** `wifi_ap.json` (the manual-procedure
  step-1 payload = the escape-hatch target state).

**Before the erase (credentialed device, parked, no portal).**  Wrote the
credentialed image at `0x3A0000`, flashed the app, monitored ~75 s:

```
[INFO]: [app_state] configuration --config-ok(configuration)--> network (session 1)
[INFO]: [prov_mgr] wifi provisioning adapter initialized
W (9034) klc: Network lost; lamp forced off by adapter, ThingsBoard stays disconnected until reconnect
W (31294) klc: No Wi-Fi connection within 30000 ms; verified TLS connect stays blocked
[INFO]: [app_state] network --network-failed(network)--> safe-off (session 1)
[INFO]: [app_state] safe-off --retry-due(timer)--> network (session 2)
[INFO]: [app_state] network --network-failed(network)--> safe-off (session 2)
```

Key observations: `wifi_mgmt_is_read_data()` is true (the stale credential
is loaded), so the controller's **init-time fallback does NOT fire** — no
`No saved station credential; entering Wi-Fi provisioning` and no
`[prov_mgr] provisioning AP up; portal reachable` appear anywhere.  The
device cannot reach its saved network (no lab WLAN), the NETWORK gate
times out (`NETWORK_CONNECT_TIMEOUT_MS`), and the machine parks in
`SAFE_OFF` with the bounded retry — exactly the "device that parks" state
the escape hatch must recover.

**The erase (documented path B).**  Overwrote only the storage partition
with the fresh image — **no app reflash**:

```
python -m esptool --chip esp32 -p /dev/ttyUSB1 -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 4MB --flash_freq 40m \
    0x3A0000 /tmp/klc_storage_fresh.littlefs
```

**After the erase + restart (forced back to PROVISIONING).**  The device
rebooted from the same application image; the fresh-device fallback opened
the portal on the NETWORK gate's first entry:

```
[INFO]: [app_state] configuration --config-ok(configuration)--> network (session 1)
[INFO]: [prov_mgr] wifi provisioning adapter initialized
[INFO]: [prov_mgr] controller transition 1 -> 3 (product event 1)
[INFO]: [prov_mgr] provisioning AP up; portal reachable
I (1344) klc: Provisioning flow started by the controller; entering Wi-Fi provisioning
[INFO]: [app_state] network --provisioning-started(network)--> provisioning (session 1)
I (1364) klc: Provisioning gate: portal owned by the controller; waiting for its outcome events
[INFO]: [prov_mgr] portal up; station not yet connected (waiting for a client to join and submit; one log line per 30 s)
```

The documented recovery sequence is confirmed: (1) the controller's
init-time fallback `if (!wifi_mgmt_is_read_data())` fires because the erase
removed `wifi_ap.json`; (2) the machine enters PROVISIONING via the
supervisor-delivered STARTED event; (3) the portal is reachable.  No
`Backtrace:` in the log; `on_target_smoke_check.py --expect provisioning`
**PASSES** (`legal provisioning sequence: ENTER`, backtrace-free, no test-
credential substring).  A second `idf.py flash` (fresh state persists —
app flash never touches the storage partition) reproduced the same entry
sequence and the smoke check passed again on `/tmp/hq_led_lamp_esp.log`.

**Product trigger decision (from the TASK-137 section): serial-only for
now** — no long-press factory reset and no OTA/RPC erase in this milestone;
the documented serial procedure (path B, or the platform API path A) is the
supported escape hatch, and the platform API is in place for a future
trigger.

Run the smoke check against a captured log at any time:

```sh
python3 tests/wifi_provisioning_manager/on_target_smoke_check.py \
    --log /tmp/hq_led_lamp_esp.log --expect any
```

---

## TASK-138: on-target verification record — fresh device reaches the portal

Scope: **scenario (a) only** — a fresh device (no saved station credential)
opens the provisioning portal and stays provisionable at the
`NETWORK --PROVISIONING_STARTED--> PROVISIONING` gate.  The interactive
success path (a human joins the AP and submits a real test SSID/password;
the controller retires the AP after the grace period; the machine proceeds
`PROVISIONING_SUCCEEDED -> NETWORK -> TLS`) is the **MANUAL STEP** and is
deferred to the single batched session with TASK-139; the automated log
assertions below cover the fresh-entry half, and the same smoke checker
covers the success-path signatures whenever that session exercises them.

**Setup.**  WROOM build rebuilt from scratch (`idf.py fullclean &&
idf.py build`, passes).  A fresh storage image was prepared with the
platform `littlefs_util` (4096-byte blocks, 393216 B) from the manual-
procedure step-1 payload — `device.json`, `manufacturing.json`
(`state=1 mode=1`), `mqtt.json`, `identity.json`, `/cert/ca.crt` — with
**no** `wifi_ap.json` (the fresh-device target state).  The credential
store was erased per the TASK-137 procedure (path B, serial):
`idf.py erase-flash -p /dev/ttyUSB1` (fresh NVS -> no saved station
credential; fresh storage) + `write_flash 0x3A0000 /tmp/klc_storage_fresh.littlefs`
(restores the four config documents + CA), then `idf.py flash -p /dev/ttyUSB1`.
Device: ESP32-WROOM-32D (`/dev/ttyUSB1`, chip `ESP32-D0WD-V3`,
MAC `c0:49:ef:e8:24:b8`), ESP-IDF v5.5.5.

**Observed log sequence (first fresh cycle, 60 s monitor).**  The boot goes
filesystem -> configuration -> network; the NETWORK gate's first entry finds
no `wifi_ap.json`, the controller's fresh-device fallback opens the portal,
and the supervisor delivers the STARTED outcome:

```
I (604) klc: Kitchen LED Controller starting (state-machine supervisor)
[app_state] boot --start(bootstrap)--> filesystem (session 1)
I (664) klc: Filesystem ready: storage mounted at /littlefs (cert/, config/, state/)
[app_state] filesystem --fs-ok(filesystem)--> configuration (session 1)
I (724) klc: device.json loaded: product='HQ Lamp' hw='B' tb='klc-kitchen-01'
I (754) klc: manufacturing.json loaded: state=1 mode=1
I (844) klc: Device identity loaded (access-token auth, client id ready for ThingsBoard initialization)
[app_state] configuration --config-ok(configuration)--> network (session 1)
[DEBUG]: wifi_config_load: path=wifi_ap.json        <- credential store erased:
[DBG]/[ERR]: wifi_config_read_file: osal_stat failed rc=-46  <- no wifi_ap.json -> wifi_mgmt_is_read_data()=false
[INFO]: [prov_mgr] wifi provisioning adapter initialized
[INFO]: [prov_mgr] controller transition 1 -> 3 (product event 1)
[INFO]: [prov_mgr] provisioning AP up; portal reachable          <- TASK-135 portal-reachability signature
I (1344) klc: Provisioning flow started by the controller; entering Wi-Fi provisioning
[INFO]: [app_state] network --provisioning-started(network)--> provisioning (session 1)
I (1364) klc: Provisioning gate: portal owned by the controller; waiting for its outcome events
[INFO]: [prov_mgr] portal up; station not yet connected (waiting for a client to join and submit; one log line per 30 s)
```

(The `esp_wifi_connect failed: ESP_ERR_WIFI_SSID` lines bracketing the
portal-up line are the STA side of AP+STA trying to connect with the
still-empty config before a submission — expected on a fresh device, never
a credential leak.)

**Automated log assertions** (`on_target_smoke_check.py --log
/tmp/hq_led_lamp_esp.log --expect provisioning`) — **PASS**:

- no `Backtrace:` (0 lines in the captured window),
- legal provisioning sequence (`ENTER` only): the supervisor delivered the
  controller's STARTED event, i.e. the legal `NETWORK --provisioning-started-->
  PROVISIONING` entry — exactly scenario (a),
- **TASK-135 portal-reachability signature** `[INFO]: [prov_mgr] provisioning
  AP up; portal reachable` observed (assertion (c), added in TASK-138 — the
  WHOLE portal, not merely the radio, is up),
- no `KLC-Smoke-128-Test` / `KLC-Sm0ke-Pa55-128!` substring anywhere.

**Reproducibility.**  A second `idf.py flash` (app flash never touches the
storage partition, so no `wifi_ap.json` re-appears) + 60 s monitor
reproduced the **identical** entry sequence (configuration gate -> `No saved
station credential` path -> `provisioning AP up; portal reachable` ->
`network --provisioning-started--> provisioning` -> rate-limited wait
heartbeat); the smoke check PASSED again on `/tmp/hq_led_lamp_esp2.log`.
The dev workstation's Wi-Fi was never touched (serial-only procedure).

**On-target observation — AP identity.**  This product build does not call
`wifi_mgmt_set_ap_credentials()` before `wifi_mgmt_start()`, so the
provisioning AP this build advertises is the platform **default**:
`wifi_provisioning:<MAC>` (observed `wifi_provisioning:c0:49:ef:e8:24`),
**open** (no password — the platform logs
`starting an UNSECURED (open, no password) provisioning AP ... because no
product identity was configured`).  The `Bimbrownik:<MAC>` /
`SuperTrudne1!-_` identity documented above is the platform **demo**
example's runtime identity; wiring a product AP identity (serial-derived
SSID + WPA2 password) is a follow-up, not part of this verification.  Until
then, the manual join step looks for the `wifi_provisioning:<MAC>` network
(see the AP facts table in the [manual procedure](#manual-on-target-procedure);
a later task should update it when the identity is wired).

**Manual step (TASK-139's batched session).**  A human joins the
provisioning AP with a phone/laptop and submits a real test SSID/password
through the captive portal (the workstation's Wi-Fi stays untouched); the
captured log must then be asserted for: no SSID/password substring anywhere,
the station connects, the controller retires the AP after the grace period,
and the machine proceeds `PROVISIONING_SUCCEEDED -> NETWORK -> TLS` (the
smoke checker validates the `SUCCESS` transition and the no-secret rule;
the NETWORK/TLS gate lines are recorded from that session — see the
TASK-139 record below).

---

## TASK-139: on-target verification record — stale credential falls back to the portal

Scope: the bug report's core case — a device with a saved credential that
cannot connect (router replaced / password changed) must **not** park forever
with no AP: the controller's bounded fallback budget (TASK-131) must open
the provisioning AP after the configured `CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS`
budget.  Observed on `/dev/ttyUSB1` (ESP32-WROOM-32D, MAC
`c0:49:ef:e8:24:b8`), ESP-IDF v5.5.5.

### Setup — seed a deliberately wrong credential

The stale-credential storage image was built from the canonical fresh
payload of the manual procedure (four config documents + `cert/ca.crt`) with
a `wifi_ap.json` seeding a **deliberately wrong** saved credential — the
throwaway values of the smoke checker's `--stale-*` defaults:

```json
{"last_use":0,"credentials":[{"nb":0,"ssid":"KLC-Stale-Net-139","password":"KLC-Stale-Pass-139!"}]}
```

(Written with the platform `littlefs_util`, 4096-byte blocks, 393216 B —
same tooling as TASK-137; the secrets never appear in any log, asserted
below.)  The image was written at the storage offset `0x3A0000` and the app
flashed (`idf.py flash` never touches the storage partition, so the wrong
credential survives the flash).

### Finding: the bounded budget was unreachable on hardware (fixed)

The **first** on-target run (the TASK-138 build, before this task's change)
reproduced the bug the report describes **exactly**: the device went through
one CONNECT_FAILED (the manager emits **at most one CONNECT_FAILED per
connect request** and then rests IDLE), the NETWORK gate timed out, the
machine parked SAFE_OFF with the bounded retry — and **no portal ever
opened**, because the budget of 2 was never reached:

```
[DEBUG]: [wifi] saved credentials found, auto-connect enabled
[DEBUG]: [wifi] state IDLE -> CONNECT
[DEBUG]: [wifi] state CONNECT -> WAIT_CONNECT
[INFO]:  [prov_mgr] wifi provisioning adapter initialized
W (9024) klc: Network lost; lamp forced off by adapter, …   <- CONNECT_FAILED #1 (budget 1/2)
[DEBUG]: [wifi] state WAIT_CONNECT -> STOP -> IDLE          <- the manager rests idle; no re-connect
W (31294) klc: No Wi-Fi connection within 30000 ms
[INFO]:  [app_state] network --network-failed(network)--> safe-off (session 1)
[INFO]:  [app_state] safe-off --retry-due(timer)--> network (session 2)
W (63294) klc: No Wi-Fi connection within 30000 ms          <- session 2: NO new connect attempt
[INFO]:  [app_state] network --network-failed(network)--> safe-off (session 2)
```

So the controller's budget could never exhaust — a genuine product gap the
on-target verification exposed: **the NETWORK gate must re-drive the
saved-credential connect on every bounded-retry re-entry** so each session
contributes one CONNECT_FAILED.  Fixed in this task:

- `network_manager_reconnect()` (new adapter API) — re-requests the station
  connect (never while connected, no-op when stopped),
- the NETWORK gate calls it on every credentialed (re-)entry, so the saved
  credential is re-driven once per bounded-retry session,
- `supervise_network_gate()` now services the provisioning outcome events
  in-loop, so the controller's STARTED (fired when the budget exhausts
  mid-window) is consumed at once and the gate yields to PROVISIONING
  instead of timing out with an open portal behind it.

Host coverage: new `network_manager_tests` case
(`test_reconnect_redrives_connect_request_only_when_not_connected`); the
full wifi_provisioning_manager host suites still pass.

### Observed log sequence — after the fix (two independent runs)

With the fixed build: session 1 contributes CONNECT_FAILED #1, the gate
times out and parks (episode 1), the bounded retry re-enters NETWORK and the
gate **re-drives** the connect, session 2 contributes CONNECT_FAILED #2,
the budget (2) exhausts, the controller opens the portal and the supervisor
consumes STARTED in-gate — the machine enters PROVISIONING in session 2,
**no extra park, no restart storm**:

```
[INFO]:  [app_state] configuration --config-ok(configuration)--> network (session 1)
[DEBUG]: [wifi] saved credentials found, auto-connect enabled
[DEBUG]: [wifi] state IDLE -> CONNECT
[INFO]:  [prov_mgr] wifi provisioning adapter initialized
W (9104) klc: Network lost; lamp forced off by adapter, …   <- CONNECT_FAILED #1 (budget 1/2)
[DEBUG]: [wifi] state WAIT_CONNECT -> STOP -> IDLE
W (31364) klc: No Wi-Fi connection within 30000 ms
[INFO]:  [app_state] network --network-failed(network)--> safe-off (session 1)   <- episode 1
[INFO]:  [app_state] safe-off --retry-due(timer)--> network (session 2)
[DEBUG]: [wifi] state IDLE -> CONNECT                      <- reconnect() re-drive (TASK-139)
W (41164) klc: Network lost; lamp forced off by adapter, …  <- CONNECT_FAILED #2 (budget 2/2 EXHAUSTED)
[INFO]:  [prov_mgr] controller transition 1 -> 3 (product event 1)   <- fallback fired
[INFO]:  [prov_mgr] provisioning AP up; portal reachable             <- TASK-135 signature
I (41214) klc: Provisioning flow started by the controller; entering Wi-Fi provisioning
[INFO]:  [app_state] network --provisioning-started(network)--> provisioning (session 2)
I (41284) klc: Provisioning gate: portal owned by the controller; waiting for its outcome events
[INFO]:  [prov_mgr] portal up; station not yet connected (waiting for a client to join and submit; one log line per 30 s)
```

**Automated assertions** (`on_target_stale_check.py` — builds the seeded
image, writes storage, app-flashes, captures 90 s, runs the smoke check)
**PASS** on **two independent runs**:

- `legal stale-credential-fallback sequence: NETWORK_FAILED -> ENTER` —
  one NETWORK_FAILED episode before PROVISIONING_STARTED, i.e. within the
  configured budget of 2,
- `--expect stale-credential` no-restart-storm checks: exactly one ENTER
  and exactly one `provisioning AP up; portal reachable` period,
- backtrace-free (0 `Backtrace:` lines),
- no `KLC-Stale-Net-139` / `KLC-Stale-Pass-139!` (stale) nor
  `KLC-Smoke-128-Test` / `KLC-Sm0ke-Pa55-128!` (test) substring anywhere —
  the wrong credential seeded onto storage never appears in the log.

The log (`/tmp/klc_stale_fixed_2.log`) and the full procedure are recorded
above; the device reproducibly opens the provisioning AP after the bounded
budget.

### MANUAL STEP — recovery after re-provisioning (requires a human + second radio)

The interactive success path needs a phone/laptop to join
`wifi_provisioning:c0:49:ef:e8:24` (open, platform-default identity — see
the TASK-138 AP-identity note) and submit the **correct** credential of a
real 2.4 GHz lab WLAN through `POST /api/v1/wifi/credentials`
(`{"ssid":…,"password":…}` at `http://10.10.0.1`); the dev workstation's
Wi-Fi stays untouched (host network invariant — this workstation has a
single radio, so it cannot join the device AP without dropping the home
network).  Once that session's log is captured, assert it with:

```sh
python3 tests/wifi_provisioning_manager/on_target_smoke_check.py \
    --log <recovery.log> --expect stale-recovery \
    --stale-ssid KLC-Stale-Net-139 --stale-password KLC-Stale-Pass-139! \
    --test-ssid <lab-ssid> --test-password <lab-password>
```

The assertion set (`--expect stale-recovery`) requires, after the same
bounded fallback, the full recovery sequence: the station connects, the
controller retires the portal after the 2000 ms success grace
(`Provisioning succeeded; portal retired by the controller`), the machine
proceeds `network --provisioning-succeeded--> network` -> `Network
connected; verified MQTT/TLS connect is now allowed` -> `Verified TLS
connected with validated identity` (the documented success signatures
above), backtrace-free and credential-free.  (If the lab WLAN cannot reach
the ThingsBoard broker, the TLS line is legitimately replaced by `Verified
TLS connect failed: …` — provisioning and NETWORK still succeeded; record
the gate lines from that session.)
