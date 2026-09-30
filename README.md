# Kitchen LED Controller

Single-channel kitchen LED controller firmware for ESP32-WROOM-32D.
Drives an external MOSFET with PWM. Connects to a local ThingsBoard instance
over MQTT with verified TLS and a per-device access token.

ThingsBoard setup, shared attributes, dashboard controls, and troubleshooting
are documented in [docs/THINGSBOARD_USER_MANUAL.md](docs/THINGSBOARD_USER_MANUAL.md).

## Prerequisites

- [ESP-IDF v5.5.4](https://github.com/espressif/esp-idf/releases/tag/v5.5.4)
- CMake ≥ 3.16

## Quick start

```bash
git clone --recurse-submodules <REPO_URL>
cd kitchen-led-controller
source ../esp-idf-v5.5.4/export.sh
idf.py build
```

## Boot sequence

The application state machine (`components/app_state`, driven by
`main/app_main.c`) owns the full boot order:

```
boot -> safe-off -> filesystem -> configuration -> Wi-Fi
     -> provisioning (controller-driven: fresh device at init, or
        exhausted-credential fallback)
     -> verified MQTT/TLS -> state sync -> online
```

The **provisioning stage** sits between the Wi-Fi gate and the TLS gate and
is owned by the **platform fallback controller** (`wifi_provisioning_controller`
in `platform/hq_platform`), not by the application state machine: the
supervisor (`main/app_main.c`) only mirrors the controller's outcome events
into the machine (`NETWORK --provisioning-started--> PROVISIONING` and back
on success) and never starts or restarts the portal itself.  The controller
opens the portal on either of **two entry paths**:

1. **Fresh device at init** — no saved station credential (`wifi_ap.json`):
   the controller opens the provisioning portal on the NETWORK gate's first
   entry.
2. **Exhausted-credential fallback** — a saved credential that fails to
   connect counts against a bounded `CONNECT_FAILED` budget
   (`CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS`, default 2 in this
   product); when the budget is spent the controller opens the portal
   instead of leaving the device parked without an AP.

The portal is a temporary access point with an HTTP portal and captive DNS
on the shared Mongoose process — this build advertises the platform-default
identity `wifi_provisioning:<MAC>` (the documented demo identity
`Bimbrownik:<MAC>` / `SuperTrudne1!-_` applies once a product AP identity is
wired; see `tests/wifi_provisioning_manager/README.md`).  After a submitted
credential connects, the controller keeps the portal up for the configured
success grace (`CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS`, default
2 s), then retires it cleanly — only the portal listeners close; MQTT/TLS
are untouched — and the boot chain continues to the TLS gate with the now
saved credential.  If the submitted connection fails before the grace
expires, the portal stays up for another attempt (grace-abort).  A
**credentialed device that connects on the first try** passes the Wi-Fi gate
straight to TLS and never enters provisioning.  A provisioning failure parks
the machine degraded (SAFE_OFF) with the existing bounded retry — never a
portal restart storm — and any parked or credential-less device is bringable
back to the portal through the **erase/re-provision escape hatch**
(platform `wifi_mgmt_erase_credentials()`, or the serial storage-partition
erase; TASK-137 in `tests/wifi_provisioning_manager/README.md`).

The policy semantics behind these entry paths, the fallback budget and the
success-grace retirement are defined by the platform controller — see
`platform/hq_platform/src/wifi_provisioning/wifi_provisioning_controller.h`
and `platform/hq_platform/examples/esp/wifi_provisioning_demo/README.md`;
this README only records how the product wires them (entry paths, budget and
grace defaults, escape hatch).

Note that provisioning sits *after* the configuration gate: a fresh device
must carry the validated configuration documents (`device.json`, `mqtt.json`,
`manufacturing.json`, `identity.json`) and the CA (`/cert/ca.crt`) on the
LittleFS `storage` partition before the network stage can decide to enter
provisioning.  On-target verification therefore flashes a pre-built storage
image alongside the app (see `tests/wifi_provisioning_manager/README.md`,
step 1).

This flow is inherently radio-level and is verified on target: see
`tests/wifi_provisioning_manager/README.md` for the manual procedure, the
expected log signatures, the controller-driven failure-mode matrix (with
the on-target TASK-138/139 records as verification evidence) and the
automated monitor-log smoke check (`on_target_smoke_check.py`, part of the
standard flash/monitor loop).

## Platform submodule

`platform/hq_platform` is pinned to commit
`e2a6d1bbd975b006981ee7ab871dec6284c45a48`.
To bump the platform, update the submodule in a dedicated MR.

## Repository structure

```
components/          Product domain components (added in later MRs)
main/                ESP-IDF application entry point
platform/hq_platform Git submodule — OSAL, Mongoose, Wi-Fi, ThingsBoard
scripts/             Manufacturing and server helper scripts
server/              Local ThingsBoard Docker Compose stack
tests/               Host-side unit and integration tests
CMakeLists.txt       Root project
partitions.csv       OTA-capable partition table
sdkconfig.defaults   WROOM-32D build defaults
```

## Targets

| Target         | Status          |
|----------------|-----------------|
| ESP32-WROOM-32D | Primary (MR 001) |
| ESP32-S3        | Planned (MR 023) |
| ESP32-C6        | Planned (MR 023) |

## License

MIT — see [LICENSE](LICENSE).
