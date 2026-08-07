---
name: thingsboard-feature-implementation
description: 'Plan and implement ThingsBoard device-client and OTA features in this repo. Use for tb_firmware_update, tb_client, telemetry, attributes, RPC, claim, provisioning, MQTT reconnect behavior, checksum verification, signature validation, OSAL-backed OTA flows, and focused unit-test updates.'
argument-hint: 'Describe the feature, affected behavior, constraints, and expected validation.'
user-invocable: true
---

# ThingsBoard Feature Implementation

Use this skill when the task is to add or change a ThingsBoard device-client or OTA capability in this repository.

## When To Use

- Implement firmware update, checksum, signature, rollback, or OTA state behavior.
- Add or change telemetry, attributes, RPC, claim, or provisioning behavior.
- Improve reconnect handling, pending-request cleanup, or protocol validation.
- Update unit tests or mocks for ThingsBoard features.

## Repository Focus

- Core modules are under `src/thingsboard/`.
- Primary tests and mocks are under `tests/thingsboard/`.
- Current examples include ThingsBoard demos under `examples/common/`.
- OTA changes should preserve the OSAL boundary and keep `osal_ota_*` responsible for write/apply behavior.

For the repository map, load [module map](./references/module-map.md).
For the implementation checklist, load [implementation checklist](./references/implementation-checklist.md).

## Procedure

1. Identify the owning module before exploring adjacent code.
2. Read only enough nearby code to form one falsifiable local hypothesis.
3. Determine the cheapest focused validation that can disconfirm that hypothesis.
4. Summarize the affected files, intended change, risks, and validation plan.
5. Ask for approval before editing if the request is broad, risky, or architecture-affecting.
6. Implement the smallest change that satisfies the requirement at the root cause.
7. Add or update focused tests for success and failure paths.
8. Run narrow validation before widening scope.
9. Report the outcome, any remaining risks, and follow-up work.

## Project Rules

- Prefer OSAL abstractions over direct platform networking or socket code.
- Prefer the existing Mongoose-based transport path when transport behavior changes.
- Avoid generated build directories.
- Do not introduce broad refactors unless they are required to unlock the feature.
- For OTA changes, keep checksum, signature, abort, finish, and rollback behavior explicit and testable.

## Expected Output

Before editing:

- Owning files and modules
- Local implementation hypothesis
- Minimal edit plan
- Focused validation plan

After editing:

- What changed
- Tests or builds run
- Remaining gaps or risks