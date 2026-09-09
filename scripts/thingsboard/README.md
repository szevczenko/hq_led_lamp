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
