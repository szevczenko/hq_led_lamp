# ThingsBoard test device and functional-test harness

Scripts and tests here validate the ThingsBoard-side protocol contract
(section 8 of `../../KITCHEN_LED_CONTROLLER_PRODUCTION_PLAN.md`) against a
real ThingsBoard tenant, using only the REST/HTTP API — no firmware or
hardware required. See `../../Server-side-api` for the tenant REST reference
and https://thingsboard.io/docs/reference/http-api/ for the device HTTP API
used to simulate a device.

## Prerequisites

- A reachable ThingsBoard tenant (default assumed: `http://home-assistance.local:8080`).
- A tenant-scoped API key with `TENANT_ADMIN` rights
  (ThingsBoard UI: **Security settings → API keys**, or reuse an existing one).
- Python 3.10+. No third-party packages are required (standard library only).

## Environment variables

### Lamp LittleFS image (`provision_lamp.sh`)

The five credential variables below are required. Copy
`device/device_credentials.example.sh` to `device/device_credentials.sh`, fill
in the values, then source the private file from the project root:

```bash
source device/device_credentials.sh
```

| Variable | Required | Description |
| --- | --- | --- |
| `WIFI_SSID` | Yes | SSID written to the initial `wifi_ap.json`. |
| `WIFI_PASSWORD` | Yes | Password written with the initial Wi-Fi credential. |
| `TB_DEVICE_NAME` | Yes | Lamp device name; written as its serial and ThingsBoard name. |
| `TB_PROVISION_DEVICE_KEY` | Yes | ThingsBoard device-provisioning key written to `provisioning.json`. |
| `TB_PROVISION_DEVICE_SECRET` | Yes | ThingsBoard device-provisioning secret written to `provisioning.json`. |
| `TB_DEVICE_PORT` | No | Serial port used to flash; default: `/dev/ttyUSB1`. |
| `TB_PAYLOAD_DIR` | No | Temporary LittleFS payload directory; default: `/tmp/klc_payload`. |
| `TB_STORAGE_IMAGE` | No | Output LittleFS image; default: `/tmp/klc_storage.littlefs`. |
| `LITTLEFS_TOOL` | No | `littlefs_util` command or executable path; default: `littlefs_util` found in `PATH`. |
| `TB_LITTLEFS_TOOL` | No | Legacy alias for `LITTLEFS_TOOL`; ignored when `LITTLEFS_TOOL` is set. |
| `TB_SKIP_FLASH` | No | Set to `1` to generate the image without flashing it; default: `0`. |

The provisioner deletes `TB_PAYLOAD_DIR` and `TB_STORAGE_IMAGE` before
creating them. Use paths that contain only disposable generated data. If
`littlefs_util` is not installed or is not in `PATH`, set `LITTLEFS_TOOL` to
its executable path.

### ThingsBoard tools and functional tests

| Variable | Used by | Description |
| --- | --- | --- |
| `TB_API_KEY` | `provision_test_device.py`, `ota_deploy.py`, functional tests | Tenant API key. Required by device provisioning and functional tests. OTA uses it in preference to its API-key file option. |
| `TB_BASE_URL` | Device provisioning, lamp reset, OTA, functional tests | ThingsBoard base URL; default: `http://home-assistance.local:8080`. A command-line `--base-url` overrides it in the command-line tools. |
| `TB_DEVICE_NAME` | `provision_lamp.sh`, `reset_provisioned_lamp.py` | Required lamp name for image provisioning; optional default for reset (otherwise pass `--device-name`). |
| `TB_TENANT_API_KEY_FILE` | `reset_provisioned_lamp.py` | Tenant API-key file; default: `thingboard_tenant_api_key` in the project root. `--api-key-file` overrides this value. |
| `TB_TEST_DEVICE_NAME` | Functional tests | Test device name; default: `klc-test-01`. |
| `RUN_TB_FUNCTIONAL_TESTS` | Functional tests | Set to `1` to enable network tests; otherwise they are skipped. `TB_API_KEY` must also be set. |

`provision_test_device.py` and `ota_deploy.py` also accept `--device-name`
or `--device` as applicable; those command-line arguments are not controlled
by `TB_DEVICE_NAME`. `ota_deploy.py` accepts `--api-key-file` (default:
`caredentials/thingboard_token`) when `TB_API_KEY` is not set.

## Provisioning the test device

```bash
export TB_API_KEY=<your tenant API key>
# export TB_BASE_URL=http://home-assistance.local:8080   # optional, this is the default
python3 provision_test_device.py
```

This finds or creates a device named `klc-test-01` (idempotent — safe to
rerun) and writes its access token to `.klc-test-01.token` next to this
script, with `0600` permissions. The token file is git-ignored
(see `../../.gitignore`) and is never printed to stdout/stderr.

Use `--device-name` to provision a differently named device, and
`--device-profile` to select a non-default device profile.

## Claim handover (`claim_handover.sh`)

Tenant-side helper that prepares a device for customer claiming: it generates
a crypto-random one-time secret and publishes `claimingData` as a
SERVER_SCOPE attribute (`POST
/api/plugins/telemetry/DEVICE/{id}/attributes/SERVER_SCOPE`, format per
`FIRMWARE_PRODUCTION_READINESS_PLAN.md` §9.5). The Customer User later calls
`POST /api/customer/device/{deviceName}/claim` with the secret (TASK-303).

```bash
scripts/thingsboard/claim_handover.sh <device-name>              # issue (or re-issue)
scripts/thingsboard/claim_handover.sh <device-name> --reissue    # explicit re-issuance
scripts/thingsboard/claim_handover.sh <device-name> --invalidate # delete claimingData
scripts/thingsboard/claim_handover.sh <device-name> --status     # claim state, never the secret
scripts/thingsboard/claim_handover.sh <device-name> --handover   # issue + print the secret to stdout
```

| Variable | Description |
| --- | --- |
| `TB_BASE_URL` | ThingsBoard base URL; default `http://home-assistance.local:8080`. |
| `TB_TENANT_API_KEY_FILE` | Tenant-admin API key file read at runtime; default `/home/dima/projects/hq_workspace/thingboard_tenant_api_key`. |
| `TB_CLAIM_TTL` | Claim validity in seconds; default 3600 (overridable with `--ttl`). |

The tenant API key and the generated secret are never printed or logged: the
secret is written to stdout only in the explicit `--handover` mode (all status
goes to stderr), so it can be piped into the out-of-band delivery channel
(waybill / label / e-mail). The smoke test (create a throwaway device, issue,
re-issue, invalidate, handover, delete the device) is described in the script
header; it is a development smoke test only and not proof of the target PE
behavior (`docs/THINGSBOARD_USER_MANUAL.md` §13.6).

## Running the functional tests

The tests are opt-in and skipped by default so they don't run without
network access or credentials:

```bash
export TB_API_KEY=<your tenant API key>
export RUN_TB_FUNCTIONAL_TESTS=1
python3 -m unittest discover -s ../../tests/thingsboard -v
```

Tests cover:

- idempotent provisioning (rerunning does not create a duplicate device),
- shared-attribute (`power`/`brightness`) round-trip via the Attributes API,
- telemetry publish (as a mock device) and read-back via the Time Series API,
- two-way RPC (`getState`) using a background thread as the mock device,
- authentication failure (bad API key) and unreachable-server failure, both
  of which must raise `ThingsBoardError` without leaking the API key.

## Secrets handling

- The tenant API key is only ever read from the `TB_API_KEY` environment
  variable; it is never written to a file by these scripts.
- Device access tokens are written only to the git-ignored `.*.token` files
  in this directory.
- Neither value is ever included in printed output, exceptions, or test
  assertions.
