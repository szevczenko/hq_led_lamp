# ThingsBoard User Manual

## 1. Device Overview

The kitchen lamp is registered in ThingsBoard as:

* **Device name:** `klc-kitchen-01`
* **MQTT endpoint:** `mqtts://home-assistance.local:8883`
* **MQTT username:** device access token
* **MQTT password:** empty
* **Required shared attributes:** `power` and `brightness`

The device keeps its output disabled until it has successfully received and validated both required shared attributes from ThingsBoard.

The shared attributes represent the persistent server-side state of the lamp. They are used by the device during initial synchronization and after reconnects or reboots.

---

## 2. Create the Device

1. Sign in to the ThingsBoard tenant as a tenant administrator.

2. Open **Entities → Devices**.

3. Select **Add new device**.

4. Set the device name to:

   `klc-kitchen-01`

5. Select the default device profile unless a project-specific device profile is already available.

6. Save the device.

7. Open the device and select **Credentials**.

8. Select **Access token** and copy the generated token into the device identity provisioning record.

   **Do not put the tenant API key into the firmware.**

The access token configured in the firmware must be the same as the access token assigned to the ThingsBoard device.

Never publish the access token in telemetry, dashboards, screenshots, logs, or source control.

---

## 3. Set the Initial Lamp State

This step is required before the device can complete its initial ThingsBoard synchronization.

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

Use the following JSON types:

* `power`: boolean
* `brightness`: integer from `0` to `100`

Do **not** enter `"false"` as a string. The correct value is the JSON boolean `false`.

After connecting to ThingsBoard, the device requests both shared attributes. When both values are received and validated successfully, the synchronization process can complete.

The expected state transition is:

```text
TLS -> SYNC -> ONLINE
```

If the response to a shared-attribute request contains `{}`, the requested response contains no matching attribute values. Check that both `power` and `brightness` exist under **Shared attributes** and that their names are spelled exactly as expected.

---

## 4. Change the Persistent Lamp State

The ThingsBoard shared attributes represent the persistent server-side state used by the device during boot and reconnect synchronization.

To change the stored lamp state manually:

1. Open the device's **Attributes → Shared attributes** tab.

2. Edit `power` and/or `brightness`.

3. Save the changes.

The device subscribes to shared-attribute updates. When a complete and valid state is received, the new state is applied to the hardware.

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
   * `pwm_duty`
   * `connection_state`
   * `uptime_ms`

  These are telemetry values. They do not edit the device's shared
  attributes.

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

9. Save the dashboard.

The exact widget names and locations may vary between ThingsBoard releases and editions. Look for widgets in the **Control**, **RPC**, or equivalent widget categories.

The important configuration is the RPC method name and JSON parameters.

Do not use a generic value widget that sends `getValue` or `setValue`. Those
methods are not implemented by this firmware. The firmware accepts only
`setPower`, `setBrightness`, `setState`, and `getState`.

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

Therefore, `data.power` is not the correct parser for the full response;
use `data.applied.power` or `data.applied.brightness` as appropriate.

---

## 6. RPC Behavior

The firmware supports the following server-side RPC methods:

| Method          | Parameters                          | Purpose                          |
| --------------- | ----------------------------------- | -------------------------------- |
| `setPower`      | `{"power": true}`                   | Enable or disable the lamp       |
| `setBrightness` | `{"brightness": 0}`                 | Set brightness from 0 to 100     |
| `setState`      | `{"power": true, "brightness": 60}` | Set both values                  |
| `getState`      | `{}`                                | Read the currently applied state |

RPC is a transient control mechanism.

An RPC command changes the currently applied hardware state and, where implemented by the firmware, publishes the resulting state as telemetry. An RPC command does **not** replace the shared attributes used for boot synchronization.

The shared-attributes card therefore will not change after an RPC command.
This is intentional: RPC is a transient command channel. To persist a new
state, edit **Attributes -> Shared attributes** and save both values.

If a state must survive reboot or reconnect, update the corresponding ThingsBoard shared attributes as well.

For example, sending:

```json
{
  "power": true,
  "brightness": 60
}
```

through `setState` changes the current hardware state, but the persistent ThingsBoard state should also be updated to:

```json
{
  "power": true,
  "brightness": 60
}
```

Otherwise, the device may restore the previous shared-attribute state after reboot or resynchronization.

---

## 7. Verify the Device

On the device serial monitor, a healthy connection should include messages similar to:

```text
MQTT connected
Connected to ThingsBoard
ThingsBoard state synchronization in progress
```

After valid shared attributes have been received and validated, the expected final transition is:

```text
sync --sync-complete(thingsboard)--> online
```

In ThingsBoard, verify that:

* **Latest telemetry** contains:

  * `power`
  * `brightness`
  * `pwm_duty`
  * `connection_state`
  * `uptime_ms`

* `connection_state` reports:

  ```text
  online
  ```

* Changing the shared attributes changes the lamp state.

* The RPC control widget sends the expected command and receives a successful RPC response.

* Rebooting the device restores the state stored in the ThingsBoard shared attributes.

---

## 8. Troubleshooting

### 8.1 Response payload is `{}`

The response to the shared-attribute request does not contain the requested attribute values.

Check:

1. The device name is `klc-kitchen-01`.
2. The attributes are stored under **Shared attributes**, not client-side or server-side attributes.
3. Both attributes exist:

   * `power`
   * `brightness`
4. The attribute names are spelled exactly as expected.
5. The values use the correct JSON types:

   * `power`: boolean
   * `brightness`: integer

Expected example:

```json
{
  "power": false,
  "brightness": 0
}
```

---

### 8.2 Device is online but the lamp stays off

Check the current shared state:

```json
{
  "power": true,
  "brightness": 60
}
```

The lamp output is intentionally disabled when:

* `power` is `false`,
* `brightness` is `0`,
* the state is invalid,
* the state is incomplete,
* synchronization has not completed,
* or the ThingsBoard connection is lost and the firmware enters its fail-safe state.

If `power` is `true` and `brightness` is greater than `0`, check the device telemetry and PWM output.

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

The exact MQTT `CONNACK` return code depends on the MQTT protocol version and broker implementation. Do not assume that a particular return code, such as `2`, uniquely identifies an invalid ThingsBoard access token. Use the ThingsBoard/server logs together with the MQTT client logs to determine the actual authentication failure.

---

### 8.4 RPC does not work

Check:

1. The dashboard entity alias points to `klc-kitchen-01`.
2. The RPC method name exactly matches one of the supported firmware methods:

   * `setPower`
   * `setBrightness`
   * `setState`
   * `getState`
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

**Get current state:**

```json
{}
```

---

## 9. State and Control Model

The lamp has two distinct control paths:

```text
                    ThingsBoard
                         |
             +-----------+-----------+
             |                       |
       Shared Attributes             RPC
             |                       |
       Persistent state        Transient command
             |                       |
             +-----------+-----------+
                         |
                      Device
                         |
                  Hardware output
```

### Shared attributes

Used for:

* initial synchronization,
* reconnect synchronization,
* restoring the state after reboot,
* storing the persistent server-side lamp state.

### RPC

Used for:

* immediate user control,
* changing the current hardware state,
* reading the current state with `getState`.

RPC does not automatically modify the shared attributes.

Therefore, if a user wants a new state to survive reboot, the corresponding shared attributes must also be updated.

---

## 10. Expected Startup Sequence

A normal startup should follow this sequence:

```text
Device boot
    |
    v
Initialize hardware
    |
    v
Establish TLS connection
    |
    v
Connect MQTT to ThingsBoard
    |
    v
Subscribe to required ThingsBoard topics
    |
    v
Request shared attributes
    |
    v
Receive power + brightness
    |
    v
Validate complete state
    |
    v
Apply lamp state
    |
    v
Publish telemetry
    |
    v
ONLINE
```

The device remains fail-safe during synchronization. It must not enable the lamp output based on incomplete or invalid state.

If synchronization fails or the connection is lost, the firmware intentionally disables the hardware output according to its fail-safe policy.
