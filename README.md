# Kitchen LED Controller

Single-channel kitchen LED controller firmware for ESP32-WROOM-32D.
Drives an external MOSFET with PWM. Connects to a local ThingsBoard instance
over MQTT with verified TLS and a per-device access token.

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
     -> provisioning (only when no saved station credential exists)
     -> verified MQTT/TLS -> state sync -> online
```

A **fresh device** (no saved station credential) enters the Wi-Fi
provisioning stage between the Wi-Fi gate and TLS: the supervisor starts
the provisioning portal — a temporary access point (`Bimbrownik:<MAC>`,
password `SuperTrudne1!-_`) with an HTTP portal and captive DNS on the
shared Mongoose process — and waits a bounded window for a phone/laptop to
submit the home SSID/password.  On success the portal stays up for the
configured grace interval (default 2 s), is retired cleanly (only the
portal listeners close; MQTT/TLS are untouched), and the boot chain
continues to the TLS gate with the now saved credential.  A
**credentialed device** passes the Wi-Fi gate straight to TLS and never
enters provisioning.  A provisioning failure parks the machine degraded
(SAFE_OFF) with the existing bounded retry — never a portal restart storm.

Note that provisioning sits *after* the configuration gate: a fresh device
must carry the validated configuration documents (`device.json`, `mqtt.json`,
`manufacturing.json`, `identity.json`) and the CA (`/cert/ca.crt`) on the
LittleFS `storage` partition before the network stage can decide to enter
provisioning.  On-target verification therefore flashes a pre-built storage
image alongside the app (see `tests/wifi_provisioning_manager/README.md`,
step 1).

This flow is inherently radio-level and is verified on target: see
`tests/wifi_provisioning_manager/README.md` for the manual procedure, the
expected log signatures, the failure-mode matrix and the automated
monitor-log smoke check (`on_target_smoke_check.py`, part of the standard
flash/monitor loop).

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
