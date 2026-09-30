# KLC Testing Guide

This document describes how to build, run, and interpret the Kitchen LED Controller test suites.
The device firmware uses `platform/hq_platform` for the HAL, OSAL, Wi-Fi, MQTT/TLS, ThingsBoard, and OTA layers.

## 1. Prerequisites

Run commands from the repository root:

```bash
cd ~/projects/hq_led_lamp
source ../esp-idf-v5.5.4/export.sh
```

The target test device is normally `/dev/ttyUSB1`. Stop `idf.py monitor` before flashing or provisioning the storage partition.

The local ThingsBoard CE test server is:

```text
http://home-assistance.local:8080
```

The tenant API key is read from `caredentials/thingboard_token`. Do not print or commit that value.
Wi-Fi and bootstrap provisioning values are read from `caredentials/device_env.sh`:

```bash
source caredentials/device_env.sh
```

## 2. Host Unit Tests

Each command creates an isolated build directory under `/tmp`.

### Lamp control

```bash
cmake -S tests/lamp_control -B /tmp/klc_lamp_control
cmake --build /tmp/klc_lamp_control
ctest --test-dir /tmp/klc_lamp_control --output-on-failure
```

Checks the hardware-independent lamp policy:

- brightness-to-duty conversion and rounding,
- active-high and active-low behavior,
- one-channel PWM behavior,
- three-channel RGB mapping,
- RGB state reporting,
- RGB colour kept at full duty resolution (e.g. `red=1` is not lost),
- initialization and deinitialization failures, including retrying only unreleased channels after partial deinit,
- grouped RGB fail-off that attempts every channel even when one fails,
- fail-off barriers and concurrent apply/fail-off behavior.

### Factory reset

```bash
cmake -S tests/factory_reset -B /tmp/klc_factory_reset
cmake --build /tmp/klc_factory_reset
ctest --test-dir /tmp/klc_factory_reset --output-on-failure
```

Checks debounce and long-press behavior:

- short presses are ignored,
- a hold longer than the configured duration triggers once,
- erase runs before restart,
- a failed erase is not retried until the button is released,
- the reset remains latched after triggering.

### ThingsBoard application, PWM mode

```bash
cmake -S tests/tb_application -B /tmp/klc_tb_application
cmake --build /tmp/klc_tb_application
ctest --test-dir /tmp/klc_tb_application --output-on-failure
```

Checks the device-side ThingsBoard application:

- shared-attribute synchronization,
- sync source priority: complete client attributes win over shared ones, partial client state falls back to shared, empty scopes use the safe default,
- one attribute request carrying both `clientKeys` and `sharedKeys`,
- applied state published as client attributes after an RPC change,
- `rssi` included in telemetry when an RSSI provider is configured,
- session and stale-response protection,
- strict JSON validation,
- RPC methods `setPower`, `setBrightness`, `setState`, and `getState`,
- RPC failure and disconnect handling,
- telemetry shape, values, rate limiting, and secret exclusion,
- OTA firmware hint handling.

### ThingsBoard application, RGB mode

```bash
cmake -S tests/tb_application \
  -B /tmp/klc_tb_application_rgb \
  -DCMAKE_C_FLAGS=-DCONFIG_KLC_LAMP_TYPE_RGB=1
cmake --build /tmp/klc_tb_application_rgb
ctest --test-dir /tmp/klc_tb_application_rgb --output-on-failure
```

In addition to the PWM checks, this build checks:

- RGB attributes `red`, `green`, and `blue`,
- strict RGB channel validation in the range 0..255,
- `setColor` RPC behavior,
- RGB telemetry and RPC responses,
- backward-compatible white defaults when no RGB triplet is present.

### MQTT/TLS configuration

```bash
cmake -S tests/mqtt_cfg -B /tmp/klc_mqtt_cfg
cmake --build /tmp/klc_mqtt_cfg
ctest --test-dir /tmp/klc_mqtt_cfg --output-on-failure
```

Runs two test executables. They check:

- strict `mqtt.json` parsing,
- verified TLS-only policy,
- hostname and port validation,
- CA and client certificate path validation,
- skip-verify rejection,
- broker URL returned by `mqtt_cfg_get_server_url()` only after a verified apply (and bounded by the caller buffer),
- fail-off on configuration errors,
- trusted TLS connection,
- unknown CA and hostname mismatch failures,
- plaintext and invalid certificate-path rejection.

### OTA manager

```bash
cmake -S tests/ota_manager -B /tmp/klc_ota_manager
cmake --build /tmp/klc_ota_manager
ctest --test-dir /tmp/klc_ota_manager --output-on-failure
```

Checks OTA lifecycle supervision, progress handling, indicator ownership, failure recovery, restart scheduling, updater initialization retries, and image confirmation (requested after the first sync, retried every second until the updater accepts it).

### Network manager

```bash
cmake -S tests/network_manager -B /tmp/klc_network_manager
cmake --build /tmp/klc_network_manager
ctest --test-dir /tmp/klc_network_manager --output-on-failure
```

Checks Wi-Fi adapter lifecycle, callback ownership, stale callback rejection, disconnect fail-off, RSSI reported only while started and connected, and credential erasure forcing onboarding (AP+STA) mode on the next start.

### Other component tests

The same pattern applies to the remaining standalone suites:

```bash
for test in app_config app_state device_identity lamp_fs wifi_provisioning_manager; do
  cmake -S "tests/$test" -B "/tmp/klc_$test"
  cmake --build "/tmp/klc_$test"
  ctest --test-dir "/tmp/klc_$test" --output-on-failure
done
```

These cover configuration schema validation, application state transitions and retry budgets, identity parsing and zeroization boundaries, LittleFS lifecycle, and Wi-Fi provisioning event handling. `tests/ota` contains on-target scripts (section 8), not a host CMake suite.

Known pre-existing host failures may occur in the Wi-Fi provisioning fallback/race scenarios. They are unrelated to RGB, OTA, or ThingsBoard protocol changes and are recorded in the repository testing notes.

## 3. hq_platform ThingsBoard Tests

Initialize the platform test dependency if needed:

```bash
git -C platform/hq_platform submodule update --init tests/unity
```

Build and run the complete platform ThingsBoard test binary:

```bash
cd platform/hq_platform
cmake -B /tmp/hqp_build \
  -DHQ_DEFCONFIG=defconfig/posix.defconfig \
  -DHQ_BUILD_TESTS=ON
cmake --build /tmp/hqp_build --target tb_tests
/tmp/hqp_build/tests/tb_tests
cd ../..
```

The platform suite covers 60 tests, including:

- ThingsBoard client lifecycle,
- MQTT publish, subscribe, reconnect, and request handling,
- telemetry and attribute payload shapes,
- combined client/shared attribute request (`tb_attributes_request`),
- provisioning and claiming,
- firmware update protocol and checksum handling,
- RPC transport behavior.

## 4. ESP-IDF Firmware Build

```bash
source ../esp-idf-v5.5.4/export.sh
idf.py build
```

The active project configuration is RGB mode with three PWM pins:

```text
CONFIG_KLC_LAMP_TYPE_RGB=y
CONFIG_KLC_LED_RGB_GPIO_R=18
CONFIG_KLC_LED_RGB_GPIO_G=19
CONFIG_KLC_LED_RGB_GPIO_B=21
```

The generated application image is:

```text
build/kitchen_led_controller.bin
```

The build also verifies the partition table, OTA slot size, bootloader rollback configuration, and link-time integration of all product components.

## 5. Flash Application Firmware

The normal flash command updates the bootloader, partition table, OTA metadata, and application image. It does not overwrite LittleFS storage at `0x3A0000`:

```bash
idf.py flash -p /dev/ttyUSB1
```

Capture startup logs:

```bash
timeout 1m idf.py monitor -p /dev/ttyUSB1 | tee /tmp/klc_boot.log
```

Expected startup evidence includes:

```text
Firmware: title=kitchen_led_controller version=...
Filesystem ready: storage mounted at /littlefs
mqtt_cfg: verified transport configured: mqtts://...
```

## 6. Rebuild and Flash LittleFS Storage

Use this only when Wi-Fi, provisioning, certificates, or configuration documents change:

```bash
source caredentials/device_env.sh
./scripts/thingsboard/provision_lamp.sh
```

The script:

- creates `device.json`, `manufacturing.json`, `mqtt.json`, and `provisioning.json`,
- copies the ThingsBoard CA certificate,
- creates `wifi_ap.json` using the current Wi-Fi values,
- builds a 384 KiB LittleFS image,
- flashes only the storage partition at `0x3A0000`,
- resets the device.

Because the script removes the previous `identity.json`, the test ThingsBoard device must be deleted first when using `Allow creating new devices`:

```bash
python3 scripts/thingsboard/reset_provisioned_lamp.py \
  --api-key-file caredentials/thingboard_token \
  --device-name klc-kitchen-99
```

Then monitor the enrollment:

```bash
timeout 2m idf.py monitor -p /dev/ttyUSB1 | tee /tmp/klc_provisioning.log
```

Expected evidence:

```text
Network connected
/provision/response ... "status":"SUCCESS"
mqtt_cfg: verified transport configured
sync source=default
applied complete valid desired state
```

## 7. ThingsBoard REST/API Verification

```bash
export TB_BASE_URL=http://home-assistance.local:8080
export TB_API_KEY="$(cat caredentials/thingboard_token)"
export DEV_NAME=klc-kitchen-99

tb() {
  curl -sS -H "Content-Type: application/json" \
    -H "X-Authorization: ApiKey ${TB_API_KEY}" "$@"
}

export DEV_ID=$(tb "$TB_BASE_URL/api/tenant/devices?deviceName=$DEV_NAME" | jq -r .id.id)

tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/attributes/CLIENT_SCOPE"
tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/attributes/SHARED_SCOPE"
tb "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEV_ID/values/timeseries?keys=power,brightness,red,green,blue,rssi,fw_state,current_fw_version"
```

For a two-way RPC test:

```bash
tb -X POST "$TB_BASE_URL/api/rpc/twoway/$DEV_ID" \
  -d '{"method":"setColor","params":{"red":255,"green":0,"blue":64},"timeout":10000}'
```

Then verify client attributes and telemetry again. A restart should restore the applied state from client attributes.

## 8. OTA Verification Through ThingsBoard

Note: bootloader rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) lives in the bootloader, which OTA never updates. Rollback only works after the bootloader built with it has been flashed once over serial (`idf.py flash`).

Build the image and deploy it through the repository helper:

```bash
python3 scripts/thingsboard/ota_deploy.py deploy \
  --device klc-kitchen-99 \
  --file build/kitchen_led_controller.bin \
  --allow-shared-profile \
  --wait 0
```

Monitor while the device is online:

```bash
timeout 5m idf.py monitor -p /dev/ttyUSB1 | tee /tmp/klc_ota.log
```

Expected sequence:

```text
DOWNLOADING
progress
image activated
restarting into the new firmware
Firmware: ... version=<new-version>
last_state=UPDATED
```

For automated log assertions:

```bash
python3 tests/ota/on_target_ota_check.py \
  --expect update \
  --from-version 1.0.2 \
  --to-version 1.0.3 \
  --deploy-file build/kitchen_led_controller.bin \
  --device klc-kitchen-99 \
  --deploy-args "--allow-shared-profile" \
  --duration 300
```

Always clear the shared firmware assignment afterward:

```bash
python3 scripts/thingsboard/ota_deploy.py unassign --device klc-kitchen-99
```

## 9. Full Regression Command

The following runs the main host suites, the platform suite, and the target build:

```bash
set -e
cd ~/projects/hq_led_lamp

for test in app_config app_state device_identity factory_reset lamp_control lamp_fs mqtt_cfg network_manager ota_manager tb_application wifi_provisioning_manager; do
  cmake -S "tests/$test" -B "/tmp/klc_$test"
  cmake --build "/tmp/klc_$test"
  ctest --test-dir "/tmp/klc_$test" --output-on-failure
done

cmake -S tests/tb_application -B /tmp/klc_tb_application_rgb \
  -DCMAKE_C_FLAGS=-DCONFIG_KLC_LAMP_TYPE_RGB=1
cmake --build /tmp/klc_tb_application_rgb
ctest --test-dir /tmp/klc_tb_application_rgb --output-on-failure

cd platform/hq_platform
cmake -B /tmp/hqp_build \
  -DHQ_DEFCONFIG=defconfig/posix.defconfig \
  -DHQ_BUILD_TESTS=ON
cmake --build /tmp/hqp_build --target tb_tests
/tmp/hqp_build/tests/tb_tests
cd ../..

source ../esp-idf-v5.5.4/export.sh
idf.py build
```

## 10. Security Notes

`caredentials/device_env.sh` contains Wi-Fi and bootstrap secrets and must remain local-only. Do not paste its contents into logs, issue trackers, or commits. Production deployments should use per-device secrets, flash encryption, Secure Boot, and a secure manufacturing process.
