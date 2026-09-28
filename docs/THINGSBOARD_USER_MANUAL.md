# ThingsBoard User Manual

## 1. Device Overview

The kitchen lamp is registered in ThingsBoard as:

* **Device name:** `klc-kitchen-01` (set per device through `TB_DEVICE_NAME`)
* **Device profile:** `lamp_controller`
* **MQTT endpoint:** `mqtts://home-assistance.local:8883`
* **MQTT username:** device access token (obtained automatically by device provisioning)
* **MQTT password:** empty
* **State keys:** `power` and `brightness`; RGB builds also use `red`, `green` and `blue`

The firmware is built either for a single-channel PWM lamp or for an RGB lamp (three PWM pins). The variant is selected in `sdkconfig` (`CONFIG_KLC_LAMP_TYPE_PWM` / `CONFIG_KLC_LAMP_TYPE_RGB`). Keys and RPC methods marked **RGB only** are ignored or rejected by PWM builds.

The device keeps its output disabled until the first state synchronization with ThingsBoard has completed. The state is restored in this order:

1. **client attributes** written by the device itself after every applied change,
2. **shared attributes** written by an operator or dashboard,
3. a safe default (lamp off) when neither scope contains a state.

---

## 2. Register the Device

### 2.1 Automatic provisioning (recommended)

The device registers itself on first connection using ThingsBoard device provisioning.

1. Open **Profiles → Device profiles → lamp_controller → Device provisioning**.
2. Select the strategy **Allow creating new devices** and save.
3. Copy the **Provision device key** and **Provision device secret**.
4. Copy `device/device_credentials.example.sh`, fill in `WIFI_SSID`, `WIFI_PASSWORD`, `TB_DEVICE_NAME`, `TB_PROVISION_DEVICE_KEY` and `TB_PROVISION_DEVICE_SECRET`, then generate and flash the LittleFS image:

   ```bash
   source device/device_credentials.sh
   ./scripts/thingsboard/provision_lamp.sh
   ```

5. On first boot the device connects with the bootstrap credentials, requests provisioning, stores the returned access token in its identity file and reconnects with it. The new device appears under **Entities → Devices** with the configured name.

The provisioning key/secret stay on the device (`provisioning.json`). If ThingsBoard rejects the stored access token three times in a row (for example after the device was deleted or its token regenerated), the firmware re-provisions itself automatically. A failed enrollment is retried after a 10-minute backoff.

### 2.2 Manual registration

Use this only when provisioning is not available.

1. Sign in to the ThingsBoard tenant as a tenant administrator.

2. Open **Entities → Devices**.

3. Select **Add new device**.

4. Set the device name to:

   `klc-kitchen-01`

5. Select the `lamp_controller` device profile.

6. Save the device.

7. Open the device and select **Credentials**.

8. Select **Access token** and copy the generated token into the device identity provisioning record.

   **Do not put the tenant API key into the firmware.**

The access token configured in the firmware must be the same as the access token assigned to the ThingsBoard device.

Never publish the access token in telemetry, dashboards, screenshots, logs, or source control.

---

## 3. Set the Initial Lamp State

This step is optional. A device without any stored state completes synchronization with the lamp off and then publishes that state as client attributes. Set shared attributes only when a device should start with a specific state.

1. Open **Entities → Devices → klc-kitchen-01**.

2. Open the **Attributes** tab.

3. Select **Shared attributes**.

4. Add the following attributes:

```json
{
  "power": false,
  "brightness": 0
}
```

RGB only: optionally add the colour as a complete triplet:

```json
{
  "power": true,
  "brightness": 60,
  "red": 255,
  "green": 128,
  "blue": 0
}
```

Use the following JSON types:

* `power`: boolean
* `brightness`: integer from `0` to `100`
* `red`, `green`, `blue`: integers from `0` to `255` (RGB only)

Do **not** enter `"false"` as a string. The correct value is the JSON boolean `false`.

The RGB channels must be sent together. A state with only some of them is rejected. A state without any of them uses white (`255/255/255`).

After connecting, the device requests the client and shared attributes in one request and selects the state:

| Response contents                                  | Applied state          | Log line                  |
| -------------------------------------------------- | ---------------------- | ------------------------- |
| complete client state                              | client attributes      | `sync source=client`      |
| no complete client state, complete shared state    | shared attributes      | `sync source=shared`      |
| both scopes empty                                  | lamp off (default)     | `sync source=default`     |
| only partial or invalid values                     | rejected, retried      | `rejected invalid/partial sync response` |

The expected state transition is:

```text
TLS -> SYNC -> ONLINE
```

Note that complete client attributes take priority. After the device has applied any state, editing a shared attribute while the device is **offline** does not override it on the next boot. See section 4 for how to change the state.

---

## 4. Change the Persistent Lamp State

The state can be changed in two ways. Both are persistent: after every applied change the device publishes the resulting state (`power`, `brightness`, and on RGB builds `red`, `green`, `blue`) as **client attributes**, which are restored after reboot or reconnect.

* **RPC** (section 6): immediate control from a dashboard or the REST API.
* **Shared attributes** while the device is online:

  1. Open the device's **Attributes → Shared attributes** tab.
  2. Edit `power`, `brightness` and, on RGB builds, `red`/`green`/`blue`.
  3. Save the changes.

  The device subscribes to shared-attribute updates. A complete and valid state is applied to the hardware and then written back as client attributes.

To inspect the state the device will restore, open **Attributes → Client attributes**.

For example:

```json
{
  "power": true,
  "brightness": 60
}
```

Example states:

* Lamp off:

  ```json
  {
    "power": false,
    "brightness": 0
  }
  ```

* Lamp on at full brightness:

  ```json
  {
    "power": true,
    "brightness": 100
  }
  ```

* Lamp on at 25% brightness:

  ```json
  {
    "power": true,
    "brightness": 25
  }
  ```

A brightness value of `0` produces an electrically disabled output even when `power` is `true`.

The effective hardware state therefore depends on both values.

---

## 5. Create a Dashboard

1. Open **Dashboards**.

2. Select **Add new dashboard**.

3. Name the dashboard:

   `Kitchen Lamp`

4. Open the dashboard and select **Edit**.

5. Add the device `klc-kitchen-01` to a dashboard entity alias.

6. Add a time-series or latest-value widget for the following telemetry keys:

   * `power`
   * `brightness`
   * `red`, `green`, `blue` (RGB only)
   * `pwm_duty`
   * `connection_state`
   * `uptime_ms`
   * `rssi` (Wi-Fi signal in dBm)

  These are telemetry values. They do not edit the device's attributes.

7. Add a control widget for the power RPC.

   Configure it to call:

   **Method:**

   `setPower`

   **Parameters when enabled:**

   ```json
   {
     "power": true
   }
   ```

   **Parameters when disabled:**

   ```json
   {
     "power": false
   }
   ```

   For a ThingsBoard Switch Control widget, use this configuration:

   * **RPC get value method:** `getState`
   * **Parse value function:**

     ```javascript
     return data && data.applied ? data.applied.power : false;
     ```

   * **RPC set value method:** `setPower`
   * **Convert value function:**

     ```javascript
     return { power: value };
     ```

8. Add a control widget for the brightness RPC.

   Configure it to call:

   **Method:**

   `setBrightness`

   **Parameters:**

   ```json
   {
     "brightness": 60
   }
   ```

   The `brightness` value must be an integer from `0` to `100`.

   For a ThingsBoard slider or brightness control widget, use:

   * **RPC get value method:** `getState`
   * **Parse value function:**

     ```javascript
     return data && data.applied ? data.applied.brightness : 0;
     ```

   * **RPC set value method:** `setBrightness`
   * **Convert value function:**

     ```javascript
     return { brightness: Math.round(value) };
     ```

   Set the slider step to `1`. Fractional brightness values such as `24.1`
   are rejected by the firmware.

9. RGB only: add a colour control (for example a button set or a custom
   widget) that calls:

   **Method:**

   `setColor`

   **Parameters:**

   ```json
   {
     "red": 255,
     "green": 0,
     "blue": 64
   }
   ```

   Each channel is an integer from `0` to `255`. Any subset of the channels
   may be sent; missing channels keep their current value, so each channel
   can have its own slider. `power` and `brightness` are not changed by
   `setColor`.

   For three separate ThingsBoard slider widgets (step `1`, range `0`–`255`),
   configure each one with its own channel key (`red`, `green` or `blue`):

   * **RPC get value method:** `getState`
   * **Parse value function** (example for `red`):

     ```javascript
     return data && data.applied ? data.applied.red : 0;
     ```

   * **RPC set value method:** `setColor`
   * **Convert value function** (example for `red`):

     ```javascript
     return { red: Math.round(value) };
     ```

10. Save the dashboard.

The exact widget names and locations may vary between ThingsBoard releases and editions. Look for widgets in the **Control**, **RPC**, or equivalent widget categories.

The important configuration is the RPC method name and JSON parameters.

Do not use a generic value widget that sends `getValue` or `setValue`. Those
methods are not implemented by this firmware. The firmware accepts only
`setPower`, `setBrightness`, `setState`, `getState` and, on RGB builds,
`setColor`.

For `setPower`, the parameters must be an object such as
`{"power": true}`. Do not send the bare JSON value `true` or `false`. For
brightness, send an integer from `0` to `100`, never `null`, a string, or a
fractional number.

The `getState` response contains nested desired and applied values. Dashboard
controls should read the applied value:

```json
{
  "success": true,
  "desired": {
    "power": true,
    "brightness": 60
  },
  "applied": {
    "power": true,
    "brightness": 60,
    "output_active": true
  }
}
```

On RGB builds both `desired` and `applied` also contain `red`, `green` and
`blue`.

Therefore, `data.power` is not the correct parser for the full response;
use `data.applied.power` or `data.applied.brightness` as appropriate.

---

## 6. RPC Behavior

The firmware supports the following server-side RPC methods:

| Method          | Parameters                          | Purpose                          |
| --------------- | ----------------------------------- | -------------------------------- |
| `setPower`      | `{"power": true}`                   | Enable or disable the lamp       |
| `setBrightness` | `{"brightness": 0}`                 | Set brightness from 0 to 100     |
| `setState`      | `{"power": true, "brightness": 60}` | Set both values; RGB builds also accept an optional complete `red`/`green`/`blue` triplet |
| `setColor`      | `{"red": 232}` or `{"red": 255, "green": 0, "blue": 64}` | RGB only: set one or more colour channels; missing channels, power and brightness are kept |
| `getState`      | `{}`                                | Read the desired and applied state |

On PWM builds `setColor` returns `{"success": false, "error": "unknown method"}`.

An RPC command changes the hardware state, publishes telemetry, and writes the resulting state to the device's **client attributes**. The state therefore survives reboot and reconnect without editing shared attributes.

The **Shared attributes** card does not change after an RPC command. The current persistent state is visible under **Attributes → Client attributes**.

If the lamp output is temporarily suspended (for example during an OTA update), a valid RPC is stored and acknowledged, written to client attributes, and applied when the output resumes.

---

## 7. Verify the Device

On the device serial monitor, a healthy connection should include messages similar to:

```text
Firmware: title=kitchen_led_controller version=1.0.3 partition=ota_0 ...
mqtt_cfg: verified transport configured: mqtts://home-assistance.local:8883 client_id=klc-kitchen-01
Verified TLS connected with validated identity; ThingsBoard session initialization allowed
[tb_app] sync source=client
[tb_app] applied complete valid desired state: power=off brightness=0
```

After the state has been applied, the expected final transition is:

```text
sync --sync-complete(thingsboard)--> online
```

In ThingsBoard, verify that:

* **Latest telemetry** contains:

  * `power`
  * `brightness`
  * `red`, `green`, `blue` (RGB only)
  * `pwm_duty`
  * `connection_state`
  * `hardware`
  * `uptime_ms`
  * `rssi`
  * `current_fw_title` and `current_fw_version`

* `connection_state` reports:

  ```text
  online
  ```

* **Client attributes** contain `power`, `brightness` (and `red`/`green`/`blue` on RGB builds) matching the applied state.

* Changing the shared attributes changes the lamp state.

* The RPC control widget sends the expected command and receives a successful RPC response.

* Rebooting the device restores the last applied state from the client attributes.

---

## 8. Troubleshooting

### 8.1 Sync fails with `rejected invalid/partial sync response`

An empty response (`{}` or empty `client`/`shared` scopes) is valid: the device applies the safe default (lamp off). A rejection means a scope contains a state that is incomplete or has wrong types, and no other scope has a complete state.

Check:

1. Both `power` and `brightness` exist together in the same scope (client or shared).
2. The attribute names are spelled exactly as expected.
3. The values use the correct JSON types:

   * `power`: boolean
   * `brightness`: integer from `0` to `100`
   * `red`, `green`, `blue`: integers from `0` to `255`, all three or none (RGB only)

Expected example:

```json
{
  "power": false,
  "brightness": 0
}
```

---

### 8.2 Device is online but the lamp stays off

Check the applied state under **Client attributes** or with `getState`:

```json
{
  "power": true,
  "brightness": 60
}
```

The lamp output is intentionally disabled when:

* `power` is `false`,
* `brightness` is `0`,
* on RGB builds, `red`, `green` and `blue` are all `0`,
* the state is invalid,
* the state is incomplete,
* synchronization has not completed,
* an OTA update is in progress,
* or the ThingsBoard connection is lost and the firmware enters its fail-safe state.

If `power` is `true` and `brightness` is greater than `0`, check the device telemetry (`pwm_duty`) and the PWM output.

---

### 8.3 MQTT authentication fails

Check the following:

1. The device is configured with the correct ThingsBoard MQTT endpoint.
2. The MQTT username contains the **device access token**.
3. The MQTT password is empty.
4. The access token belongs to `klc-kitchen-01`.
5. The token has not been regenerated or revoked in ThingsBoard.
6. The device is connecting to the expected ThingsBoard instance.

Do **not** use the tenant API key as the MQTT username.

The firmware treats MQTT `CONNACK` codes 4/5 (MQTT 3.1.1) and 0x86/0x87 (MQTT 5) as an authentication rejection and logs:

```text
ThingsBoard credentials rejected (1/3)
```

After three consecutive rejections it logs `Re-provisioning scheduled after auth rejection` and enrolls again with the provisioning key/secret (section 2.1). If enrollment also fails (wrong key/secret, or the profile does not allow creating new devices), it is retried after 10 minutes. Use the ThingsBoard/server logs together with the device logs to confirm the cause.

---

### 8.4 RPC does not work

Check:

1. The dashboard entity alias points to `klc-kitchen-01`.
2. The RPC method name exactly matches one of the supported firmware methods:

   * `setPower`
   * `setBrightness`
   * `setState`
   * `getState`
   * `setColor` (RGB only)
3. The parameters are valid JSON.
4. The parameter names and types are correct.

Examples:

**Set power:**

```json
{
  "power": true
}
```

**Set brightness:**

```json
{
  "brightness": 75
}
```

**Set complete state:**

```json
{
  "power": true,
  "brightness": 75
}
```

**Set colour (RGB only, one or more channels):**

```json
{
  "red": 232
}
```

```json
{
  "red": 255,
  "green": 0,
  "blue": 64
}
```

**Get current state:**

```json
{}
```

---

## 9. State and Control Model

```text
                    ThingsBoard
                         |
             +-----------+-----------+
             |                       |
     Shared attributes              RPC
     (operator input)         (immediate control)
             |                       |
             +-----------+-----------+
                         |
                      Device  --- applies state ---> Hardware output
                         |
                         v
                 Client attributes
          (last applied state, restored at boot)
```

### Shared attributes

Used for:

* setting the state from the ThingsBoard UI or REST API while the device is online,
* providing the initial state of a device that has no client attributes yet.

### RPC

Used for:

* immediate user control from dashboards,
* reading the desired and applied state with `getState`.

### Client attributes

Written only by the device after every applied change (from sync, shared-attribute update or RPC). They are the first source used during boot and reconnect synchronization, so the last applied state survives reboot.

---

## 10. Expected Startup Sequence

A normal startup should follow this sequence:

```text
Device boot
    |
    v
Initialize hardware (output off)
    |
    v
Connect Wi-Fi (or start the provisioning portal when no credentials exist)
    |
    v
Establish verified TLS connection and connect MQTT
    |
    v
(First boot only) provision device and reconnect with the new access token
    |
    v
Subscribe to shared-attribute updates and RPC
    |
    v
Request client + shared attributes
    |
    v
Select source: client -> shared -> default (off)
    |
    v
Apply lamp state and publish client attributes
    |
    v
Publish telemetry
    |
    v
ONLINE (confirm a freshly updated OTA image)
```

The device remains fail-safe during synchronization. It must not enable the lamp output based on incomplete or invalid state.

If synchronization fails or the connection is lost, the firmware intentionally disables the hardware output according to its fail-safe policy.

---

## 11. Firmware Updates (OTA)

The device reports its running firmware as the telemetry keys `current_fw_title` and `current_fw_version`.

To update the firmware:

1. Build the new image with a bumped `version.txt`.
2. Upload it as an OTA package of type **Firmware** with title `kitchen_led_controller` and assign it to the device (or its profile), for example:

   ```bash
   python3 scripts/thingsboard/ota_deploy.py deploy \
     --device klc-kitchen-01 \
     --file build/kitchen_led_controller.bin \
     --allow-shared-profile
   ```

3. The device downloads and verifies the image and reports progress in the `fw_state` telemetry key (`DOWNLOADING`, `DOWNLOADED`, `VERIFIED`, `UPDATING`, `UPDATED` or `FAILED`). The lamp output is off during the update.
4. After the restart, the new image is confirmed only after the first successful ThingsBoard synchronization. If the new image fails to reach that point, the bootloader rolls back to the previous image on the next reset.
5. If the package was assigned to a shared profile, remove the assignment afterwards:

   ```bash
   python3 scripts/thingsboard/ota_deploy.py unassign --device klc-kitchen-01
   ```

---

## 12. Factory Reset

Hold the factory-reset button (default GPIO0, the **BOOT** button on development boards, active-low) for 5 seconds. The lamp blinks white three times, then the device:

1. turns the output off,
2. erases the stored Wi-Fi credentials,
3. clears the saved runtime state under `/state`,
4. restarts into the Wi-Fi provisioning portal.

The ThingsBoard identity and provisioning credentials are kept, so the device reconnects as the same ThingsBoard device after new Wi-Fi credentials are entered. The button is ignored during an OTA update. The GPIO, polarity and hold time are configured under **Factory reset** in `menuconfig`. The button GPIO must not be one of the lamp output pins; the build fails with `conflicts with KLC_FACTORY_RESET_GPIO` otherwise.
