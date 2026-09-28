# KLC – ESP32 firmware completion plan (production readiness)

This plan comes from comparing `KITCHEN_LED_CONTROLLER_PRODUCTION_PLAN.md` with the current code.
The device backend is **hq_platform** (`platform/hq_platform`). Platform changes are allowed
and are explicitly marked as **[PLATFORM]**.

## Decisions

| Topic | Decision |
| --- | --- |
| Control from Flutter | RPC is the main channel; the firmware reports the applied state as **client attributes** and restores it from them after reconnect |
| Hardware variant | Selected in `sdkconfig`: **PWM (1 channel)** or **RGB (3 channels)** |
| Telemetry | Add `rssi`; no temperature sensor |
| Factory reset | GPIO configurable in `sdkconfig`, default **GPIO0** (BOOT button), long press |
| Rejected credentials | `provisioning.json` is kept after enrollment; re-provisioning after N CONNACK rejections |
| Flash encryption / Secure Boot | Documented production step only – **DO NOT run on the test board** (eFuse, irreversible) |
| Provisioning strategy | Unchanged: *Allow creating new devices* |

## Stage order

| # | Stage | Priority |
| --- | --- | --- |
| 0 | Test environment and baseline | – |
| 1 | RPC + client attributes as the state source | critical |
| 2 | Recovery after a rejected token | high |
| 3 | Factory reset (GPIO from Kconfig) | high |
| 4 | OTA: rollback, health confirmation, `current_fw_*` | high |
| 5 | `rssi` telemetry | medium |
| 6 | Lamp variant PWM / RGB | medium |
| 7 | Broker address from configuration only | medium |
| 8 | Production security (documentation only) | before production |
| 9 | Final verification: new image via OTA from ThingsBoard + E2E | – |

Every stage ends with: host tests green → build → flash → device log → verification through the ThingsBoard API.

---

## Stage 0 – Test environment and baseline

### 0.1 Build, flash, logs

```bash
cd ~/projects/hq_led_lamp
source ../esp-idf-v5.5.4/export.sh
idf.py build
idf.py flash -p /dev/ttyUSB1          # does not overwrite the storage partition (0x3A0000)
timeout 1m idf.py monitor -p /dev/ttyUSB1 | tee /tmp/klc_boot.log
```

The boot log must contain the version line (already present in `app_main()`):

```text
Firmware: title=kitchen_led_controller version=<version.txt> partition=...
```

### 0.2 Updating configuration files on the ESP32 (storage partition)

Only when a stage changes the file format in `/config` or requires a fresh enrollment:

```bash
source caredentials/device_env.sh
./scripts/thingsboard/provision_lamp.sh     # the port must not be held by the monitor
```

> Note: the script builds the **whole** LittleFS image from scratch, so `identity.json` is lost.
> With *Allow creating new devices*, ThingsBoard rejects provisioning of an existing device.
> Delete the device in ThingsBoard before flashing the storage again:
>
> ```bash
> python3 scripts/thingsboard/reset_provisioned_lamp.py \
>   --api-key-file caredentials/thingboard_token --device-name klc-kitchen-99
> ```

### 0.3 ThingsBoard API helper commands

```bash
export TB_BASE_URL=http://home-assistance.local:8080
export TB_API_KEY="$(cat caredentials/thingboard_token)"      # tenant key, do not print
export DEV_NAME=klc-kitchen-99
tb() { curl -sS -H "Content-Type: application/json" -H "X-Authorization: ApiKey ${TB_API_KEY}" "$@"; }

export DEV_ID=$(tb "$TB_BASE_URL/api/tenant/devices?deviceName=$DEV_NAME" | jq -r .id.id)

# attributes and telemetry
tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/attributes/CLIENT_SCOPE"
tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/attributes/SHARED_SCOPE"
tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/timeseries?keys=power,brightness,rssi,fw_state,current_fw_version"

# RPC (two-way)
tb -X POST "$TB_BASE_URL/api/rpc/twoway/$DEV_ID" \
   -d '{"method":"getState","params":{},"timeout":10000}'
```

### 0.4 Host tests

```bash
# application components
cmake -S tests/<x> -B /tmp/klc_<x> && cmake --build /tmp/klc_<x> && ctest --test-dir /tmp/klc_<x> --output-on-failure

# platform (ThingsBoard)
git -C platform/hq_platform submodule update --init tests/unity
cd platform/hq_platform && cmake -B /tmp/hqp_build -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_TESTS=ON \
  && cmake --build /tmp/hqp_build --target tb_tests && /tmp/hqp_build/tests/tb_tests
```

Known failures unrelated to this plan: `wifi_provisioning_fallback_flow_tests`, `wifi_provisioning_race_tests`.

### 0.5 ThingsBoard source code (optional)

For verifying server behaviour (provisioning an existing device, claiming, CE permissions):

```bash
git clone https://github.com/thingsboard/thingsboard.git ~/projects/thingsboard
git -C ~/projects/thingsboard checkout v4.3.1   # test server version
```

Key locations: `DeviceProvisionServiceImpl`, `ClaimDevicesServiceImpl`, `CustomerUserPermissions`.

**Baseline DoD:** the device boots with the current version, is `ONLINE`, and `getState` returns the state.

---

## Stage 1 – RPC + client attributes as the state source

**Problem:** RPC does not persist the state; after reconnect the shared attributes win
(`components/tb_application/include/tb_application.h`, section *Server-side RPC control*).

### Target model

1. After **every** successful output change (RPC set, shared-attribute update, synchronization)
   the firmware publishes client attributes with the **applied** state:
   `{"power":..,"brightness":..}` (+ `r`,`g`,`b` in the RGB variant, stage 6).
2. On every connection the synchronization sends **one** request with `clientKeys` and `sharedKeys`:
   - complete and valid client attributes → apply them (last state set by the user),
   - otherwise complete shared attributes → apply them and publish as client attributes,
   - otherwise (new device) → `power=false`, `brightness=CONFIG_KLC_DEFAULT_BRIGHTNESS_PERCENT`,
     publish as client attributes and treat as synchronized.
3. Shared-attribute updates during a session still apply live (override by an admin/dashboard).
   A shared-attribute change made while the device is offline does **not** win over client attributes.
   Describe this rule in the component README.
4. The fail-off rule is unchanged: the output stays off until synchronization completes.

### Changes

- **[PLATFORM]** `src/thingsboard/tb_attributes.c/.h`: new function
  `tb_attributes_request(client, client_keys, n_client, shared_keys, n_shared, cb, user_data, timeout_ms)`
  (one `v1/devices/me/attributes/request/{id}` request with `clientKeys` and `sharedKeys`; response
  `{"client":{..},"shared":{..}}`). The existing `request_client`/`request_shared` stay as wrappers.
  Tests in `tests/thingsboard/` (payload format, both scopes, a single scope, timeout, disconnect).
- `components/tb_application/tb_application.c`:
  - synchronization through `tb_attributes_request()` with the source-selection rule described above,
  - `publish_applied_state()` through `tb_attributes_send_json()` after every successful change
    (only after `lamp_control_apply_state() == LAMP_OK`, only while connected),
  - log of the synchronization source: `sync source=client|shared|default`,
  - keep the line `[tb_app] applied complete valid desired state` (checked by `tests/ota/on_target_ota_check.py`).
- Update `tb_application.h` and `README.md` (RPC is no longer *transient*).

### Host tests (`tests/tb_application`)

- RPC `setPower`/`setBrightness`/`setState` → publish on `v1/devices/me/attributes` with the applied state,
- failed/invalid RPC → no publish,
- synchronization: complete client → client; partial client → shared; neither → default + publish,
- invalid types/ranges in client → fall back to shared (never enable the output from bad data),
- disconnect → no publish, no queueing.

### Device verification

```bash
tb -X POST "$TB_BASE_URL/api/rpc/twoway/$DEV_ID" \
   -d '{"method":"setState","params":{"power":true,"brightness":40},"timeout":10000}'
tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/attributes/CLIENT_SCOPE?keys=power,brightness"
# expected: power=true, brightness=40
```

Then reset the board (EN button) and capture the log:

```bash
timeout 1m idf.py monitor -p /dev/ttyUSB1 | tee /tmp/klc_e1.log
grep -E "sync source=client|applied complete valid desired state" /tmp/klc_e1.log
```

After the restart `getState` returns `power=true, brightness=40`. Live override test:

```bash
tb -X POST "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/SHARED_SCOPE" -d '{"power":false,"brightness":10}'
# client attributes must change to false/10
```

**DoD:** a state set through RPC survives an ESP32 restart and a broker restart.

---

## Stage 2 – Recovery after a rejected token

**Problem:** `provisioning.json` is deleted after enrollment (`tb_provisioning_enroll()`), and a CONNACK
rejection is not distinguished from a network error – the device stays in SAFE_OFF permanently.

### Changes

- **[PLATFORM]** MQTT transport (`mqtt_app`, `tb_client`): expose the last CONNACK code,
  e.g. `int mqtt_app_get_last_connack_code(void)`, and the “not authorized” classification
  (MQTT 3.1.1: 4/5; MQTT 5: 0x86/0x87). Tests in the platform `tests/` against the transport mock.
- `components/mqtt_cfg`: new status `MQTT_CFG_ERR_AUTH_REJECTED` returned by `mqtt_cfg_connect()`
  instead of the generic `MQTT_CFG_ERR_CONNECT` when the broker rejected the credentials.
- `components/tb_provisioning`: do **not** delete `provisioning.json` after a successful enrollment;
  overwrite `identity.json` atomically (tmp + rename, as today). Update the tests.
- `main/app_main.c` (TLS gate):
  - counter of consecutive `MQTT_CFG_ERR_AUTH_REJECTED` results (reset after a successful connection),
  - after `CONFIG_KLC_AUTH_REJECT_REPROVISION_THRESHOLD` (default 3) → bootstrap mode
    (`tb_provisioning_load()` + `tb_provisioning_enroll()`); the old `identity.json` stays until the new one
    has been written,
  - a `FAILURE` response from ThingsBoard → backoff `CONFIG_KLC_REPROVISION_BACKOFF_MS`
    (default 10 min), no fast loop,
  - the token never reaches the logs.
- `main/Kconfig.projbuild`: the two new options listed above.

> **Server limitation (to be confirmed in `~/projects/thingsboard`, `DeviceProvisionServiceImpl`):**
> with *Allow creating new devices*, provisioning a device with an existing name ends with `FAILURE`.
> Automatic recovery therefore only works after the device is deleted in ThingsBoard, which also removes
> the Customer assignment (the customer has to claim it again). Describe this in `docs/THINGSBOARD_USER_MANUAL.md`.

### Host tests

- `tests/tb_provisioning` (new or extended): the file is kept after enrollment; atomic write; a `FAILURE` response does not overwrite the identity,
- `tests/mqtt_cfg`: mapping of CONNACK not-authorized → `MQTT_CFG_ERR_AUTH_REJECTED`,
- `tests/app_state` (if the logic goes into the state machine): threshold, counter reset, backoff.

### Device verification

```bash
# 1. revoke the token (change credentialsId)
tb "$TB_BASE_URL/api/device/$DEV_ID/credentials" > /tmp/cred.json
jq '.credentialsId = "revoked-'$(date +%s)'"' /tmp/cred.json > /tmp/cred_new.json
tb -X POST "$TB_BASE_URL/api/device/credentials" -d @/tmp/cred_new.json >/dev/null

timeout 3m idf.py monitor -p /dev/ttyUSB1 | tee /tmp/klc_e2a.log
# expected: N x "auth rejected", then "re-provisioning", "FAILURE", "backoff"

# 2. delete the device -> the device recreates itself
python3 scripts/thingsboard/reset_provisioned_lamp.py --api-key-file caredentials/thingboard_token --device-name $DEV_NAME
timeout 3m idf.py monitor -p /dev/ttyUSB1 | tee /tmp/klc_e2b.log
export DEV_ID=$(tb "$TB_BASE_URL/api/tenant/devices?deviceName=$DEV_NAME" | jq -r .id.id)
# expected: enrollment OK, ONLINE, new DEV_ID
```

For this test it helps to temporarily reduce the backoff in `sdkconfig`.

**DoD:** a revoked token does not hang the device; after the device is deleted in ThingsBoard the lamp comes back online without reflashing the storage.

---

## Stage 3 – Factory reset (GPIO from Kconfig)

### Behaviour

- Holding the button ≥ `CONFIG_KLC_FACTORY_RESET_HOLD_MS` (default 5000 ms) **after boot**.
  GPIO0 is a strapping pin: held during reset it starts download mode, which is expected.
- Indication: fast lamp blinking (like the OTA indicator).
- Action: force the output off → `wifi_mgmt_erase_credentials()` (**[PLATFORM]**, exists)
  → delete `/state/*` → restart.
- **Kept:** `identity.json`, `provisioning.json`, `mqtt.json`, `device.json`, `manufacturing.json`, `/cert`.
  After the reset the device opens the Wi-Fi portal and returns to the **same** ThingsBoard device.
  Ownership changes through claiming, not through reset.

### Changes

- `main/Kconfig.projbuild`, “Factory reset” menu:
  `KLC_FACTORY_RESET_ENABLE` (y), `KLC_FACTORY_RESET_GPIO` (0), `KLC_FACTORY_RESET_ACTIVE_LOW` (y),
  `KLC_FACTORY_RESET_HOLD_MS` (5000).
- New component `components/factory_reset`:
  - `hal_gpio_init()` as an input with pull-up (**[PLATFORM]** HAL, unchanged),
  - `factory_reset_poll(now_ms)` called from the supervisor loop every 50 ms (no ISR, host-testable),
  - debounce, single trigger, injected `on_indicator`, `erase`, `restart` callbacks (as in `ota_manager`).
- `main/app_main.c`: init after `lamp_control_init()`, `poll` in `supervise_iteration()` in all states
  except `OTA`.

### Host tests (`tests/factory_reset`)

Short press ignored; bounce ignored; a hold triggers exactly once; order
(output off → erase → restart); erase failure → no restart and an error log; blocked during OTA.

### Device verification

1. Run `timeout 1m idf.py monitor -p /dev/ttyUSB1 | tee /tmp/klc_e3.log` and hold BOOT for ≥ 5 s.
2. In the log: `factory reset`, restart, `Provisioning flow started` (portal, AP visible on the phone).
3. Configure Wi-Fi through the portal → device `ONLINE` with the **same** `DEV_ID`
   (`tb "$TB_BASE_URL/api/tenant/devices?deviceName=$DEV_NAME" | jq -r .id.id`), without re-provisioning.

**DoD:** reset clears Wi-Fi and keeps the identity; pin and hold time are configurable in `sdkconfig`.

---

## Stage 4 – OTA: rollback, health confirmation, `current_fw_*`

### Changes

- `sdkconfig.defaults`: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
  Requires reflashing the bootloader (`idf.py flash` does it automatically).
- `components/ota_manager`: after `APP_EVENT_SYNC_COMPLETE` call
  `tb_firmware_update_confirm_health()` (**[PLATFORM]**, exists, currently unused).
  A new image that does not reach synchronization rolls back to the previous one after reset
  (application watchdog → FATAL → reset).
- **[PLATFORM]** `src/thingsboard/tb_firmware_update.c`: in `fw_report_state()` also publish
  `current_fw_title`/`current_fw_version` (ThingsBoard standard); `fw_title`/`fw_version` stay for compatibility.
  Test in `tests/thingsboard/tb_tests_provision_claim_fwu.c`.
- `tests/ota/on_target_ota_check.py`: new scenario `--expect rollback` (an image that does not confirm itself
  – e.g. a build with `confirm_health` deliberately disabled – returns to the old version after restart).

### Host tests

`tests/ota_manager`: confirm called exactly once after synchronization; no confirm in SAFE_OFF/FATAL.

### Device verification

During stage 9 (OTA update) additionally:

```bash
grep -E "confirm|rollback|pending verify" /tmp/klc_ota.log
tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/timeseries?keys=current_fw_title,current_fw_version,fw_state"
```

**DoD:** the image confirms itself after synchronization; an unconfirmed image rolls back to the previous one; ThingsBoard shows `current_fw_version`.

---

## Stage 5 – `rssi` telemetry

### Changes

- `components/network_manager`: `int network_manager_get_rssi(int *dbm)` via
  `wifi_mgmt_get_rssi()` (**[PLATFORM]**, exists); error when not connected.
- `components/tb_application`: `rssi` field in the telemetry record (omitted when unknown);
  buffer size `TB_APPLICATION_TELEMETRY_MAX_BYTES` checked; field documentation updated.
- The RSSI source is injected through `tb_application_config_t` (callback), so host tests do not depend on Wi-Fi.

### Host tests

`tests/tb_application`: `rssi` present and correct; no source → field omitted; JSON still size-bounded.

### Device verification

```bash
tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/timeseries?keys=rssi"
# negative value, changes every CONFIG_KLC_TELEMETRY_PERIOD_MS
```

---

## Stage 6 – Lamp variant: PWM / RGB

### Kconfig (`main/Kconfig.projbuild`)

```text
choice KLC_LAMP_TYPE
    config KLC_LAMP_TYPE_PWM   "Single-channel PWM lamp"   (default)
    config KLC_LAMP_TYPE_RGB   "RGB lamp (3 x PWM)"
endchoice
KLC_LED_PWM_GPIO                         # PWM
KLC_LED_RGB_GPIO_R / _G / _B             # RGB, depends on KLC_LAMP_TYPE_RGB
KLC_LED_PWM_FREQUENCY_HZ / _RESOLUTION_BITS / _ACTIVE_LOW  # shared
```

File `sdkconfig.defaults.rgb` as an example RGB variant configuration
(`idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.rgb" build`).

### Changes

- `components/lamp_control`:
  - `lamp_state_t` extended with `uint8_t r, g, b` (0..255); for PWM the fields are ignored and reported as absent,
  - `lamp_control_config_t` with a channel array (1 or 3 pins) and a channel count,
  - channel duty = `brightness × colour / 255` with overflow-safe arithmetic,
  - fail-off applies to **all** channels; a failure on any channel turns the others off,
  - **[PLATFORM]** the PWM HAL supports multiple pins; check LEDC channel allocation in the ESP backend
    (`src/hal/platforms/esp`) and add a test if missing.
- `components/tb_application`:
  - RGB: new RPC method `setColor` `{"r":0..255,"g":..,"b":..}`, `setState` accepts optional `r,g,b`,
    `getState` returns the colour,
  - PWM: `setColor` → `unknown method`,
  - client/shared attributes and telemetry include `r`,`g`,`b` only in the RGB variant,
  - client attribute `lamp_type: "pwm"|"rgb"` published after connecting (Flutter selects the UI).
- `main/app_main.c`: `init_lamp_output()` selects the configuration according to `CONFIG_KLC_LAMP_TYPE_*`.

### Host tests

- `tests/lamp_control`: 1 and 3 channels; duty mapping; fail-off of all channels; colour ranges,
- `tests/tb_application`: built twice (define `KLC_LAMP_TYPE_RGB` on/off); `setColor` in both variants; synchronization with `r,g,b`.

### Device verification

- PWM build (default) → regression of stages 1 and 5.
- RGB build (only when an RGB board is available):

```bash
tb -X POST "$TB_BASE_URL/api/rpc/twoway/$DEV_ID" -d '{"method":"setColor","params":{"r":255,"g":0,"b":64},"timeout":10000}'
tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/attributes/CLIENT_SCOPE?keys=lamp_type,r,g,b"
```

---

## Stage 7 – Broker address from configuration only

- `main/app_main.c`, `initialize_thingsboard_client()`: remove `"mqtts://home-assistance.local:8883"`;
  the URL is built from `mqtt.json` (`hostname`, `port`, `tls_mode`) through an `mqtt_cfg` accessor
  (e.g. `mqtt_cfg_get_server_url(char *buf, size_t len)`).
- Host test in `tests/mqtt_cfg`: URL building, buffer truncation/overflow → error.
- Verification: the boot log shows the host from `mqtt.json`; the device is `ONLINE`.

---

## Stage 8 – Production security (documentation only, NOT on the test board)

Describe the production-line procedure in `docs/`, without running it:

- Flash Encryption in *Release* mode + Secure Boot v2 (keys outside the repository, HSM/offline),
- NVS encryption or moving `identity.json` to an encrypted partition,
- OTA image signing (consistent with Secure Boot),
- rotation of `provision_device_secret` per production batch,
- disabling JTAG and UART download mode (eFuse) in the production build.

---

## Stage 9 – Final verification: new image via OTA from ThingsBoard

### 9.1 Preparing the versions

```bash
cat version.txt                 # e.g. 1.0.2 = version on the device (FROM)
source ../esp-idf-v5.5.4/export.sh
idf.py build && idf.py flash -p /dev/ttyUSB1
timeout 1m idf.py monitor -p /dev/ttyUSB1 | tee /tmp/klc_from.log
grep "Firmware: title=kitchen_led_controller version=1.0.2 " /tmp/klc_from.log

echo 1.0.3 > version.txt        # new version (TO)
idf.py build                    # NO flash; image: build/kitchen_led_controller.bin
```

### 9.2 Deploying through ThingsBoard and observing

Automatically (monitor + deploy + log assertions):

```bash
python3 tests/ota/on_target_ota_check.py --expect update \
  --from-version 1.0.2 --to-version 1.0.3 \
  --deploy-file build/kitchen_led_controller.bin \
  --device klc-kitchen-99 --deploy-args "--allow-shared-profile" --duration 300
```

Or manually:

```bash
python3 scripts/thingsboard/ota_deploy.py deploy --device klc-kitchen-99 \
  --file build/kitchen_led_controller.bin --allow-shared-profile      # monitor in a second terminal
python3 scripts/thingsboard/ota_deploy.py status --device klc-kitchen-99
```

### 9.3 Criteria

- log: `DOWNLOADING ... version=1.0.3` → `image activated` → restart →
  `Firmware: title=kitchen_led_controller version=1.0.3` → `last_state=UPDATED`,
- stage 4: image confirmed (`confirm`), no rollback,
- ThingsBoard: `fw_state=UPDATED`, `current_fw_version=1.0.3`,
- stage 1: the lamp state from before the update is restored (`sync source=client`).

### 9.4 Cleanup (mandatory)

```bash
python3 scripts/thingsboard/ota_deploy.py unassign --device klc-kitchen-99
```

The `lamp_controller` profile is shared with the inactive `klc-kitchen-01`, which is why `--allow-shared-profile` and `unassign` are needed.

### 9.5 Lifecycle E2E: claiming + Customer (no firmware changes)

Checks whether the firmware from stages 1–6 works in a Customer User context (CE permissions):

```bash
# Customer + user (tenant)
CUST_ID=$(tb -X POST "$TB_BASE_URL/api/customer" -d '{"title":"KLC Test Customer A"}' | jq -r .id.id)
USER_ID=$(tb -X POST "$TB_BASE_URL/api/user?sendActivationMail=false" -d '{
  "email":"klc.test.a@example.com","authority":"CUSTOMER_USER",
  "customerId":{"id":"'$CUST_ID'","entityType":"CUSTOMER"}}' | jq -r .id.id)
ACT=$(tb "$TB_BASE_URL/api/user/$USER_ID/activationLink" | sed 's/.*activateToken=//')
read -rs -p "Customer password: " CUST_PASS; echo
curl -sS -X POST "$TB_BASE_URL/api/noauth/activate" -H "Content-Type: application/json" \
  -d '{"activateToken":"'$ACT'","password":"'$CUST_PASS'"}' >/dev/null

# claimingData (server-side, tenant); the value is a JSON string
SECRET=$(openssl rand -hex 16)
EXP=$(( ($(date +%s) + 3600) * 1000 ))
tb -X POST "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/attributes/SERVER_SCOPE" \
  -d '{"claimingData":"{\"secretKey\":\"'$SECRET'\",\"expirationTime\":'$EXP'}"}'

# Customer User login -> claim -> RPC as the customer
JWT=$(curl -sS -X POST "$TB_BASE_URL/api/auth/login" -H "Content-Type: application/json" \
  -d '{"username":"klc.test.a@example.com","password":"'$CUST_PASS'"}' | jq -r .token)
cu() { curl -sS -H "Content-Type: application/json" -H "X-Authorization: Bearer $JWT" "$@"; }
cu -X POST "$TB_BASE_URL/api/customer/device/$DEV_NAME/claim" -d '{"secretKey":"'$SECRET'"}'
cu -X POST "$TB_BASE_URL/api/rpc/twoway/$DEV_ID" -d '{"method":"setState","params":{"power":true,"brightness":55},"timeout":10000}'
cu "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/attributes/CLIENT_SCOPE?keys=power,brightness"

# return the device to the Tenant
cu -X DELETE "$TB_BASE_URL/api/customer/device/$DEV_NAME/claim"
```

Criteria: claim OK, RPC as a Customer User works (otherwise the CE permission model must change),
client attributes are visible to the customer, after `DELETE` the device returns to the Tenant.
Finally delete the test user and Customer (`DELETE /api/user/{id}`, `DELETE /api/customer/{id}`).

---

## Checklist

- [ ] Stage 0 – baseline
- [ ] Stage 1 – RPC + client attributes ([PLATFORM] `tb_attributes_request`)
- [ ] Stage 2 – credential recovery ([PLATFORM] CONNACK code)
- [ ] Stage 3 – factory reset (GPIO0 by default, Kconfig)
- [ ] Stage 4 – rollback + confirm + `current_fw_*` ([PLATFORM] `tb_firmware_update`)
- [ ] Stage 5 – `rssi`
- [ ] Stage 6 – PWM / RGB
- [ ] Stage 7 – broker URL from configuration
- [ ] Stage 8 – production security documentation
- [ ] Stage 9 – OTA from ThingsBoard + claiming E2E
