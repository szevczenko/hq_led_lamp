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
