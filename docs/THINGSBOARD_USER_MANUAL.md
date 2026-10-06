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
---

## 13. Handing the Device Over to the Customer (Claiming)

This section documents the chosen claiming flow: how a manufactured and provisioned lamp (registered under the tenant, section 2.1) is handed over to the end customer as a ThingsBoard **Customer** entity. Claiming is a server-side transfer of ownership after provisioning, not an initial registration step.

### 13.1 Chosen method: server-side claiming via `claimingData`

The product uses **server-side claiming** with the `claimingData` attribute. The production process writes `claimingData` as a SERVER_SCOPE attribute of the device, and the Customer User completes the transfer with the Customer Claiming endpoint.

Reasons:

* The lamp has no screen or local UI that could safely display a one-time secret, and the firmware must not generate or store a claiming secret.
* The Wi-Fi provisioning portal exists but does **not** expose the secret and is not a channel for claiming data.
* Device-side claiming would require the device to generate the secret itself and someone to read it from the device — there is no secure channel for that on this product.

The flow requires only ThingsBoard configuration and the production process; no firmware or `hq_platform` changes are needed.

### 13.2 Actors and sequence

```text
production process / tenant (tenant admin)
    |
    | 1. device already provisioned and registered under the tenant (section 2.1)
    v
production backend
    | 2. generates a crypto-random one-time secret, unique per device
    | 3. writes claimingData (SERVER_SCOPE attribute) via the REST API as tenant admin
    v
out-of-band channel (waybill / label / e-mail)
    | 4. secret delivered to the customer, one-time, with a defined validity period
    v
Customer User (ThingsBoard PE)
    | 5. POST /api/customer/device/{deviceName}/claim with the secret
    v
device transferred to the Customer
    |
    | 6. (customer return / re-issue) DELETE /api/customer/device/{deviceName}/claim
    v
device returns to the tenant (reclaim)
```

Step by step:

1. **Production/tenant:** the device is provisioned exactly as in section 2.1 and exists as a tenant device with its MQTT access token.
2. **`claimingData`:** the production process generates the secret and writes the `claimingData` SERVER_SCOPE attribute (see 13.3):
   ```bash
   curl -X POST "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$DEVICE_ID/attributes/SERVER_SCOPE" \
     -H "Content-Type: application/json" \
     -H "X-Authorization: ApiKey <tenant-admin-api-key-from-secure-store>" \
     -d '{"claimingData":"{\"secretKey\":\"<one-time-secret>\",\"expirationTime\":<expiration-ms>}"}'
   ```
   Never put a real API key or a real secret into scripts, documentation, logs, or source control; read keys from a secure store at runtime.
3. **Out-of-band secret delivery:** the secret reaches the customer through a production channel (shipping label / waybill / e-mail). It is one-time and has a defined validity period.
4. **Claim by Customer User:** the Customer User calls `POST /api/customer/device/{deviceName}/claim` with `{"secretKey": ...}` (a Customer User JWT is required; the tenant API key cannot perform the claim).
5. **Reclaim:** `DELETE /api/customer/device/{deviceName}/claim` returns the device to the tenant. Its interaction with the lamp's factory reset (section 12) is an unresolved assumption (13.6).

### 13.3 Claiming secret lifecycle

* **Generation:** by the production process / tenant admin after manufacturing, crypto-random, unique per device.
* **Storage:** as the SERVER_SCOPE attribute `claimingData` (`{"secretKey": "...", "expirationTime": <ms>}`) in ThingsBoard PE, with a short validity via `expirationTime`; never in firmware, telemetry, or source code. The value is also **stored by ThingsBoard itself in the server-side audit log**: writing `claimingData` via the REST API creates an `ATTRIBUTES_UPDATED` audit entry whose `actionData` contains the full attribute value including `secretKey` (verified on the test instance, CE 4.3.1.3, 2026-10-06 — see `docs/THINGSBOARD_CLAIMING_E2E_TEST.md` §11 finding 8). The audit log must therefore be treated as a secret-bearing store: access-controlled and not shipped off-platform, and `claimingData` should be invalidated (claimed / expired / re-issued) as soon as the handover completes.
* **Device binding:** the attribute is stored on a specific device ID; the claim endpoint verifies the secret per device.
* **Delivery:** out-of-band channel (waybill / label / e-mail), one-time, with a defined validity period.
* **Invalidation:** the claim is one-time (the secret is consumed); `expirationTime` expires it; a wrong claim is rejected; re-issuance means the tenant admin overwrites `claimingData` with a new secret; reclaim via `DELETE /api/customer/device/{deviceName}/claim` returns the device to the tenant.
* **Logging:** the secret never appears in device logs, in `claim_handover.sh`/`claim_customer.sh` output (except the explicit `--handover` stdout channel), or in this document (verified by grepping the captured device logs and the script outputs, CE 4.3.1.3). The exception is the ThingsBoard audit log described under "Storage" — operational logging must not add the secret anywhere else.

### 13.4 Trust boundaries

* **Firmware `hq_led_lamp` (no functional changes):** provisioning and MQTT connectivity remain unchanged. The firmware neither initiates nor completes the claim and never knows the claiming secret. Post-claim operation (RPC/telemetry) must require no firmware action; this is confirmed by a regression check, not by a code change. The post-claim lifecycle behavior — reboot, rejected/lost MQTT credentials, and the automatic re-enrollment of section 2.1 — is unverified with respect to device ownership and is listed as unresolved assumption 13.6.6. No local UI is added and the Wi-Fi portal is not modified.
* **ThingsBoard PE (configuration, not code):** device profile, `claimingData` attribute, Customer + Customer User entities, and the `POST/DELETE /api/customer/device/{deviceName}/claim` endpoints. Edition/version-dependent settings must be confirmed on the target PE (13.6).
* **Backend/production process:** generates the per-device secret, writes `claimingData` via the REST API as tenant admin, delivers the secret out-of-band, and logs events without the secret value. The production process and customer handle the plaintext secret; ThingsBoard PE also stores and processes it as `claimingData`, so access to that attribute and its tenant-admin/API credentials must be protected.
* **Customer User:** completes the claim and afterwards has the operational access granted to Customers on the target PE (telemetry, attributes, RPC, OTA — see the unresolved assumptions in 13.6).
* **`hq_platform`:** no changes. `tb_claim_device()` remains in the platform code but is not part of the product flow (13.7).

### 13.5 Provisioning/MQTT credentials are not the claiming secret

* The provisioning device key/secret (`TB_PROVISION_DEVICE_KEY`, `TB_PROVISION_DEVICE_SECRET`, section 2.1) and the MQTT access token are **never** used as the claiming secret and do not play that role.
* Per the assumed PE behavior, claiming does not change MQTT credentials (13.6): the device keeps connecting with the same access token after the transfer.

### 13.6 Assumed ThingsBoard version/edition and unresolved assumptions

Target edition/version: **ThingsBoard PE (Professional Edition) ≥ 3.5/3.6**.

The repository facts and the development smoke tests are **not** proof of the target PE behavior. The following must be confirmed on the target PE (https://thingsboard.io/docs/pe/user-guide/claiming/):

1. The `claimingData` format (`secretKey` + `expirationTime` in ms) and the `POST`/`DELETE /api/customer/device/{deviceName}/claim` endpoints were verified in code/tests on CE; confirm them on PE.
2. MQTT access token behavior after the claim: the PE documentation indicates that claiming does **not** change MQTT credentials (the device keeps connecting with the same token) — confirm on the target PE version.
3. Visibility of telemetry/attributes/RPC/OTA for the Customer User after the claim (PE vs CE permissions) — test on PE.
4. Whether PE requires claiming to be enabled at the tenant/profile level — to be confirmed.
5. Reclaim (`DELETE .../claim`) and its interaction with the lamp's factory reset (section 12) — to be confirmed on PE.
6. Post-claim lifecycle (reboot, rejected/lost MQTT credentials, automatic provisioning): the plan assumes that after the claim the lamp keeps working with **no firmware action** — reconnecting with the unchanged access token after a reboot and, if the token is later rejected or lost (three consecutive rejections trigger automatic re-enrollment, section 2.1), re-provisioning exactly as before the claim. Whether any of those events changes the device's ownership (Customer assignment / claimed state) is **not** verified. Target-PE regression check: (a) after a successful claim, reboot the lamp and confirm it reconnects with its unchanged token to the same device entity and stays assigned to the Customer; (b) reject or delete the stored access token and let automatic provisioning/re-enrollment run, then confirm what device entity is created, whether the provisioning key/secret still belong to the tenant, whether the new entity is assigned to the Customer, and that neither case silently changes ownership or detaches a claimed device from its Customer.

The lifecycle smoke test in `FIRMWARE_PRODUCTION_READINESS_PLAN.md` §9.5 (server-side `claimingData` + Customer User claim + reclaim) was executed on a development instance (possibly CE) and is a smoke test only; it is not proof of the target PE behavior.

#### Verification status (2026-10-06, TASK-305 smoke run)

The TASK-304 scenario was executed end to end as a **smoke run** on the development/test instance
(`http://home-assistance.local:8080`, confirmed **ThingsBoard CE 4.3.1.3**) with the **physical
lamp**; the full record is in `docs/THINGSBOARD_CLAIMING_E2E_TEST.md` §10–§11. Confirmed as facts
on CE (this proves nothing about PE):

* provisioning/`claimingData`/claim/reclaim endpoints work as described in 13.2 and 13.3;
* the claim is one-time (retry after a successful claim is rejected), `expirationTime` and wrong
  secrets are rejected, re-issuance = overwriting `claimingData` with a new secret works;
* after the claim the lamp reboots and reconnects with the **unchanged** MQTT access token to the
  **same** device entity (no firmware action, no re-provisioning — matches the PE documentation
  assumption 13.6.2);
* the Customer User, after the claim, can read telemetry/client attributes and execute two-way
  RPC (`setState` applied by the lamp, verified in the device log);
* device logs and script outputs never contain the claiming secret; **however the ThingsBoard
  server-side audit log stores the full `claimingData` value** (see 13.3 "Storage") — a finding
  from the smoke run;
* lost credentials (device deleted server-side) → MQTT rejection ×3 → automatic re-provisioning
  creates a **new tenant-owned** device entity with a new access token, exactly as documented in
  section 2.1.

**PE remains unverified.** No ThingsBoard PE environment is available in the current test
environment (the only reachable instance is the CE dev instance); the target **PE ≥ 3.5/3.6**
edition/version, the PE-specific assumptions 13.6.1–13.6.6, and the factory-reset-button step
(§6.10 of the E2E document — requires physical button access) must still be confirmed on a real
PE instance with the physical lamp. TASK-305 therefore cannot be marked done.

##### PE provisioning attempt (2026-10-06, review round)

A genuine attempt to obtain a PE environment for the required physical-lamp run was made and is
recorded in `docs/THINGSBOARD_CLAIMING_E2E_TEST.md` §10.1 (facts, not assumptions):

* the only reachable ThingsBoard is the CE dev instance (LAN scan of 192.168.1.0/24, no other
  TB HTTP/MQTT port open; `/api/license` → HTTP 404 on the dev instance), and there is no shell
  access (`ssh` refused, no Docker context) to install PE on the dev host;
* the target PE image `thingsboard/tb-pe-node:4.3.1.6PE` (Professional Edition 4.3.1.6,
  published 2026-09-28) exists on Docker Hub and was pulled; it satisfies the assumed target
  "PE ≥ 3.5/3.6";
* PE 4.3.x refuses to run without a license (`TB_LICENSE_SECRET`, verified in the image's own
  `conf/thingsboard.conf` / `thingsboard.yml`; no default or offline/trial fallback), and the
  license portal (https://license.thingsboard.io) issues trial/commercial licenses only through
  an interactive signup (e-mail verification + Google reCAPTCHA) — not completable from this
  environment (no e-mail inbox, no captcha capability), and no license material exists in the
  workspace;
* consequence: **the physical-lamp run on PE is not executable here**, assumptions 13.6.1–13.6.6
  stay unverified, and **TASK-305 remains open** for an environment with a reachable licensed
  ThingsBoard PE instance.

#### Firmware regression confirmation (TASK-306, 2026-10-06)

Firmware regression check after the claim: **no firmware code changes are required**. The standard
lamp test was run on the physical lamp (`/dev/ttyUSB1`) exactly per the `test_commands` of the
project configuration: `idf.py fullclean && idf.py build`, then
`idf.py flash -p /dev/ttyUSB1 && timeout 1m idf.py -p /dev/ttyUSB1 monitor`, with the monitor
output captured to `/tmp/hq_led_lamp_esp.log` and read (grepped, not assumed):

* **Build/flash:** `kitchen_led_controller v0.1.1` builds and flashes cleanly (no warnings
  fatal, partition fits, 37% free).
* **No backtrace/panic:** `grep -E 'Backtrace|Guru Meditation|abort\(\)|PANIC|assert failed'`
  → no matches in `/tmp/hq_led_lamp_esp.log` (and in the RPC-phase log, see below); the only
  "failure" line is the normal debug probe `Failed to stat path "/mqtt.json"` during startup.
* **Normal operation, state machine:** `Firmware: title=kitchen_led_controller version=0.1.1`;
  `mqtt_cfg: verified transport configured: mqtts://home-assistance.local:8883
  client_id=klc-kitchen-99`; `Verified TLS connected with validated identity`;
  `MQTT connected` / `[tb] Connected to ThingsBoard`; `sync source=client` →
  `sync --sync-complete(thingsboard)--> online (session 1)` — the expected
  `TLS -> SYNC -> ONLINE` sequence (section 10).
* **Telemetry:** cross-checked server-side via the test-instance API
  (`http://home-assistance.local:8080`, tenant API key read at runtime from
  `/home/dima/projects/hq_workspace/thingboard_tenant_api_key`): fresh telemetry from this run
  is present for device `klc-kitchen-99` (`connection_state=online`, `uptime_ms≈93 s` matching
  the fresh boot, `rssi=-37`, `power=false`, `brightness=100`, `pwm_duty=0`, timestamp of this
  run), and the client attributes (`power`, `brightness`, `red/green/blue`) are readable.
* **RPC handling:** a live two-way RPC was executed while monitoring the device:
  `POST /api/rpc/twoway/{deviceId}` with `setState {power:true, brightness:60}` returned
  `{"success":true,"applied":{...}}`; the device log shows
  `MQTT RX topic=v1/devices/me/rpc/request/0 payload={"method":"setState",...}` and the state
  was applied; the restore RPC (`power:false, brightness:100`) was applied the same way and the
  client attributes returned to the pre-test values. No errors/backtrace in the RPC-phase log.
* **Device identity unchanged:** the device entity `klc-kitchen-99` was not recreated by this
  regression run (same entity id and `createdTime` as before, `lamp_controller` type,
  tenant-owned/public customer, `provisionState=provisioned` — the value from the previous
  provisioning, not refreshed by any new provisioning). It reconnected with its stored
  ACCESS_TOKEN (server credential type `ACCESS_TOKEN`, token value never printed); a successful
  TLS/MQTT connect with the stored identity proves the device token still matches the server
  credential, and **no re-provisioning occurred** in this run (no `credentials rejected` /
  `Re-provisioning scheduled` / `/provision/response` log lines).
* **MQTT access token after claim:** the non-changing behaviour of the MQTT access token after a
  claim is **confirmed on the development instance (CE 4.3.1.3)** by the TASK-305 smoke run
  (post-claim reboot: same `client_id`, same device id, same credentials, no re-provisioning —
  E2E record §6.6) and by this regression run (connect with the stored token, `sync -> online`).
  It is **not confirmed on PE**: the target **PE ≥ 3.5/3.6** assumption 13.6.2 remains
  unverified because no licensed PE instance is reachable from this environment (see the PE
  provisioning attempt above). The statement "the MQTT access token does not change after claim"
  in the manual therefore holds as *confirmed on CE / documented PE behaviour, to be re-verified
  on the target PE*.
* **Firmware does not initiate the claim / does not store the claiming secret:** confirmed —
  the regression run produced only provisioning/MQTT/telemetry/RPC traffic; no
  `v1/devices/me/claim` topic activity, no claiming-secret material in the device logs, and no
  firmware code path invoking claiming (13.1, 13.4, 13.7). The firmware and its configuration
  are unchanged from the pre-claim state.
* **`hq_platform` unchanged:** the platform code remains unchanged in this task —
  `platform/hq_platform` is untouched (`tb_claim_device()` stays in `hq_platform`, unused in the
  lamp product flow, 13.7); no commit inside `hq_platform` and no `git.repositories` change to
  the automation configuration were needed because no platform change is required.

The result: the claimed/handed-over lamp operates post-claim with **no firmware action** —
connectivity, provisioning, telemetry, client attributes, and two-way RPC all work exactly as
before the claim. The only remaining unpinned item is the PE-specific verification
(assumptions 13.6.1–13.6.6) on a real licensed PE instance.

### 13.7 Device-side `tb_claim_device()` is not used in the product flow

`platform/hq_platform/src/thingsboard/tb_claim.c` provides `tb_claim_device()`, which publishes `{"secretKey": "...", "durationMs": ...}` to `v1/devices/me/claim` and returns only the result of the **MQTT publish**. It has no callback confirming that the device was transferred to the Customer — publish success is not claim success. The lamp's product flow (`main/`) does **not** invoke claiming, so `tb_claim_device()` is unused in the product.

If the product conditions change (for example the lamp gains a local UI/QR capable of safely displaying a one-time secret, or device-initiated claiming becomes a requirement), the device-side helper would need to be reinterpreted (contract: publish-only, no claim confirmation) and the change would extend into `hq_platform` as a separate repository, together with the `git.repositories` configuration of the automation.

### 13.8 Rollout order and rollback

Order: decision documentation (this section) → production script → integration tests on PE → firmware regression confirmation → manual update. The firmware does not change, so the functional rollback is removing `claimingData` / reclaiming the device to the tenant (`DELETE .../claim`); already-provisioned lamps keep working unchanged, because claiming is a server-side operation on an existing device.

The operational runbook — rollout order with task references, the safe order of production operations, handling of already-provisioned lamps, rollback, expired/lost-secret recovery, and the ThingsBoard version/edition verification summary — is section 14.

---

## 14. Device Handover Runbook

Operational companion to section 13: the safe order of operations for handing a provisioned lamp
over to a ThingsBoard Customer, the handling of already-provisioned lamps, rollback, recovery from
an expired or lost secret, and the current verification status of the ThingsBoard version/edition
assumptions. It applies to the production target **ThingsBoard PE ≥ 3.5/3.6** — the
version/edition summary and the (still open) PE verification are in 14.6.

### 14.1 Rollout order (TASK-301 → TASK-306)

The claiming rollout is a sequence of dependent tasks; each task's deliverable is a precondition
for the next in the planned order. One recorded deviation: TASK-306's regression was executed on
CE **before** TASK-305, because it needs only the dev instance and the physical lamp. That CE run
was performed out of the planned PE sequence and does **not** satisfy or unblock TASK-305 ‚Äî the
PE integration run remains open and must still be executed against the licensed target PE instance
before mass handover (14.6). Nothing in the rollout changes the firmware or `hq_platform`:

| Step | Task | Deliverable | Status |
| --- | --- | --- | --- |
| 1 | TASK-301 — decision documentation | §13.1–13.8: chosen method (server-side `claimingData`), actors/sequence, secret lifecycle, trust boundaries | done |
| 2 | TASK-302 — production script | `scripts/thingsboard/claim_handover.sh` (issue / re-issue / invalidate / status / handover) | done |
| 3 | TASK-303 — Customer User claim/reclaim helper | `scripts/thingsboard/claim_customer.sh` (claim / reclaim / `setup-test-customer`) | done |
| 4 | TASK-304 — E2E documentation | `docs/THINGSBOARD_CLAIMING_E2E_TEST.md` (PE scenario + expected results) | done |
| 5 | TASK-305 — integration test on PE | PE run record; executed as a **CE smoke run** on the dev instance with the physical lamp (E2E §10–11) | **open** — no licensed PE instance reachable; PE verification pending |
| 6 | TASK-306 — firmware regression confirmation | no firmware changes; post-claim regression on the physical lamp (reconnect with unchanged token, telemetry, two-way RPC, identity unchanged, no re-provisioning) | done (on CE) |

The manual update is this runbook (§14, the final step of the rollout order of §13.8).
Production can start with the tenant-side tools (steps 1–3 and 6); step 5 guards the **first**
PE production run and must be executed against the licensed target PE instance before mass
handover, because the CE smoke run is not PE proof (14.6).

### 14.2 Safe order of production operations

The only safe order for every handover is:

```text
1. provisioning  ->  2. claimingData  ->  3. handover (claim by Customer User)
```

Step by step:

**1. Provisioning (once per device, section 2.1).** The lamp is manufactured and provisioned: it
exists as a tenant device with its MQTT access token and has proven connectivity
(`TLS -> SYNC -> ONLINE`, sections 7 and 10).

**2. `claimingData` (production process / tenant admin).** At sale time, issue the one-time secret
and keep it in memory for the delivery channel. Issue **once**: any later issue (`--ttl`,
`--reissue`, `--handover`) overwrites `claimingData` and permanently invalidates a secret that was
already delivered:

```bash
# production side — issue once and capture ONLY the secret (stdout); memory-only, never a file:
SECRET_VAL="$(scripts/thingsboard/claim_handover.sh klc-kitchen-01 --ttl 3600 --handover 2>/dev/null)"
# verify (optional, side-effect free): the attribute is present and when it expires — not the secret:
scripts/thingsboard/claim_handover.sh klc-kitchen-01 --status
```

The secret is generated crypto-random per device and stored as the SERVER_SCOPE attribute
`claimingData` (13.3); ThingsBoard also records its value in the server-side audit log (14.4). The
tenant API key is read at runtime from `TB_TENANT_API_KEY_FILE`; neither the key nor the secret is
printed or logged — only `--handover` prints the secret (to stdout), and `--status` never shows it.

**3. Handover (out-of-band delivery + Customer User claim).** Deliver the secret captured in step 2
(`$SECRET_VAL`) through the production channel (waybill / label / e-mail), then the Customer User
completes the transfer in their own environment. The Customer User must supply
`TB_CUSTOMER_USERNAME` and `TB_CUSTOMER_PASSWORD` before the call; enter the password without
placing it in shell history (e.g. `read -rsp`):

```bash
# customer side — the Customer User's own shell (NOT the tenant/production shell):
export TB_CUSTOMER_USERNAME='customer-user@example.com'
read -rsp 'Customer User password: ' TB_CUSTOMER_PASSWORD && export TB_CUSTOMER_PASSWORD  # not in shell history
printf '%s' "$SECRET_VAL" | scripts/thingsboard/claim_customer.sh --secret-stdin klc-kitchen-01 claim   # exit 0 on success
unset TB_CUSTOMER_PASSWORD SECRET_VAL   # memory-only values die with the shell
```

`claim_customer.sh` logs in with those credentials to obtain a Customer User JWT — the tenant API
key is not accepted by the claim endpoint (§13.2). The test helper
`scripts/thingsboard/claim_customer.sh setup-test-customer` (test instance only) can create/reuse
the test Customer User with the tenant API key, but production handover always uses the Customer
User's own login.

Guard rails for the safe order:

* **Never write `claimingData` before provisioning.** The claim endpoint operates on an existing
  device entity; a lamp that is not provisioned cannot be claimed. If `claim_handover.sh` reports
  `device ... was not found`, provision the device first (section 2.1).
* **Never deliver the secret before `claimingData` is written.** The claim succeeds only while a
  valid `claimingData` attribute exists; delivering a secret for a device without one forces an
  unnecessary re-issue (14.4).
* **Never use the provisioning key/secret or the MQTT access token as the claiming secret** (13.5).
  They are not the claiming secret and do not play that role.
* **After the claim, expect no firmware action.** The lamp keeps operating with its unchanged MQTT
  access token (14.6, assumption 2). Verify the device now appears under the Customer and that
  telemetry/RPC still work — the TASK-306 regression confirmed this on the physical lamp.

### 14.3 Already-provisioned lamps (installed base)

* **No changes required.** Claiming is a server-side operation on an existing device: it does not
  alter provisioning credentials, MQTT connectivity, firmware, or factory-reset behavior (sections
  12, 13.4). Already-provisioned, already-delivered lamps keep working unchanged.
* **Claim on demand.** For each lamp that is sold, apply §14.2 steps 2–3 at sale time.
  There is no need to prepare `claimingData` earlier: leave it absent until the handover, then
  issue with a TTL that matches the delivery channel (short for label/e-mail delivery, longer only
  when delivery latency requires it).
* **Do not re-provision or re-flash an already-provisioned lamp for handover.** That would
  create/change a device entity or token for no gain; the handover itself never touches the lamp
  (TASK-306).

### 14.4 Rollback

The functional rollback of a handover is always server-side and always leaves the lamp itself
untouched. The lamp's MQTT access token and device entity survive every rollback action — it
reconnects as the same tenant device with the same token; no firmware action is needed.

Situation → action → result:

| Situation | Action | Result |
| --- | --- | --- |
| `claimingData` written, secret **not yet delivered** | `scripts/thingsboard/claim_handover.sh <device> --invalidate` | `claimingData` removed (idempotent); device stays with the tenant; no claim possible |
| secret delivered but **claim not completed** | `--invalidate` (abandon the delivery) **or** re-issue and re-deliver (`--reissue` / `--handover`) | abandoned: no claim possible; re-issued: only the newest secret works |
| device **claimed** by the Customer, must return | `scripts/thingsboard/claim_customer.sh <device> reclaim` (`DELETE /api/customer/device/{deviceName}/claim`) | device returns to the tenant; the consumed secret stops working |
| secret **compromised, expired or lost** | `--reissue` (overwrites `claimingData` with a new secret) | old secret invalid; a fresh secret is issued for new delivery (14.5) |
| **complete rollback** to the pre-claim state | `--invalidate`, then (if claimed) `reclaim` | device back with the tenant and no active `claimingData` |

Notes:

* **`--invalidate` is idempotent** — re-running it on a device without `claimingData` is a safe
  no-op.
* **`reclaim` requires a Customer User login (JWT)** — the tenant API key cannot reclaim
  (§13.2, `scripts/thingsboard/README.md`). For a device that was never claimed, rollback is
  complete with `--invalidate` alone.
* **Audit-log residue:** writing `claimingData` stores the full attribute value (including
  `secretKey`) in the ThingsBoard server-side audit log (13.3); invalidating the attribute does
  not erase that historical audit entry. Treat the audit log as a secret-bearing store.

### 14.5 Expired or lost secret

* **Check first:** `scripts/thingsboard/claim_handover.sh <device> --status` shows whether
  `claimingData` is present and when it expires — never the secret.
* **Expired** (`expirationTime` passed): the claim is rejected (E2E §6.3; `claim_customer.sh`
  exit 4 when the server reports the reason). Re-issue and deliver again:

  ```bash
  scripts/thingsboard/claim_handover.sh <device> --ttl <seconds> --handover   # new secret to stdout only
  ```

* **Lost before delivery:** re-issue (overwrite) with a fresh secret and re-deliver; the old secret
  is invalid the moment `claimingData` is overwritten.
* **Lost after delivery, claim not completed:** re-issue and deliver the new secret; only the
  newest `claimingData` value is accepted.
* **Claim already completed:** the secret is consumed (one-time, 13.3) — there is nothing to
  re-issue for that delivery. If the device must change hands again, reclaim it (14.4) and re-issue
  for the new handover.
* **Never reuse or log secrets:** each re-issue generates a new crypto-random secret; secrets
  appear only in the out-of-band delivery and — while valid — in the ThingsBoard audit log
  (13.3). Do not put them in tickets, logs, or source control.

### 14.6 ThingsBoard version/edition and assumption verification status

**Version/edition summary:**

* **Development/test instance** (all smoke runs): **ThingsBoard CE 4.3.1.3** at
  `http://home-assistance.local:8080` — results obtained there are smoke tests only and are not
  proof of the target PE behavior.
* **Target production edition:** **ThingsBoard PE ≥ 3.5/3.6** (the PE image
  `thingsboard/tb-pe-node:4.3.1.6PE` pulled during TASK-305 satisfies this assumption but requires
  a license; no licensed PE instance is reachable from the development environment — E2E
  §10.1, §11.10). **No PE run exists yet.** The confirmed PE version/edition of the
  running production instance must be recorded here after the first PE verification (TASK-305).

**Verification status of assumptions 1–5 (§13.6):**

Legend: ✔ = confirmed on the dev instance (CE smoke run / TASK-306 regression); ⛔ = PE
unverified (TASK-305 open).

| # | Assumption (§13.6) | Status on CE 4.3.1.3 (dev) | Status on target PE ≥ 3.5/3.6 |
| --- | --- | --- | --- |
| 1 | `claimingData` format (`secretKey` + `expirationTime` in ms) and `POST`/`DELETE /api/customer/device/{deviceName}/claim` endpoints | ✔ working end to end (E2E §6.1–6.9) | ⛔ unverified |
| 2 | MQTT access token unchanged after claim | ✔ lamp rebooted and reconnected with the same token/entity, no re-provisioning (E2E §6.6; TASK-306) | ⛔ unverified |
| 3 | Customer User visibility of telemetry/attributes/RPC/OTA | ✔ telemetry, client attributes and two-way RPC readable/executable by the Customer User (E2E §6.7); OTA not tested | ⛔ unverified |
| 4 | Claiming enabled at tenant/profile level | ✔ claim worked without explicit claiming-related enablement (E2E §11.4) | ⛔ unverified |
| 5 | Reclaim (`DELETE .../claim`) and factory-reset interaction | ✔ reclaim and re-issuance work (E2E §6.9); factory-reset-button step not executed (no physical button access) | ⛔ unverified |

Additional smoke-run findings (E2E §11) relevant to operations: the ThingsBoard audit log
stores `secretKey` while valid (§13.3, treat as a secret-bearing store); CE rejects
wrong/expired claims with a generic `FAILURE` body, so `claim_customer.sh`'s reason-specific exit
codes (3/4/6) must be re-verified on PE; post-claim lost credentials (device deleted server-side)
trigger automatic re-provisioning to a **new tenant-owned** entity exactly as for a never-claimed
device. Assumption 13.6.6 (post-claim lifecycle) is tracked separately in §13.6 and E2E
§11 item 6.

Because assumptions 1–5 are PE-unverified, **TASK-305 remains open**: before mass production
handover, execute the E2E scenario (`docs/THINGSBOARD_CLAIMING_E2E_TEST.md`) against the licensed
target PE instance, record the confirmed PE version/edition above, and re-verify the assumptions.
