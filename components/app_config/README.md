# app_config — versioned product configuration service (TASK-108)

Validated, recoverable product configuration for the kitchen LED
controller.  Owns the two product documents below; Wi-Fi stays behind the
Wi-Fi storage adapter and is deliberately out of scope, and **lamp state is
never persisted** (ThingsBoard is the desired-state authority in release 1).

| Logical path                     | Content                                                        |
|----------------------------------|----------------------------------------------------------------|
| `/config/device.json`            | `schema_version`, `product`, `hardware_revision`, `serial`, `thingsboard_name` |
| `/config/device.json.tmp`        | atomic-replacement staging copy (removed after each commit)     |
| `/config/device.json.good`       | last-known-good copy used for recovery                          |
| `/config/manufacturing.json`     | `schema_version`, `manufacturing_state`, `credential_mode`      |

## Bounded schemas (v1)

Every product-owned document carries an integral `schema_version`.
Unknown and duplicate members are rejected (strict schemas), so a secret
cannot be smuggled into a managed document.

- `product`: string, 1..32 printable ASCII characters
- `hardware_revision`: 1..16 printable ASCII characters
- `serial`: 1..32 characters, `A-Z a-z 0-9 -` only
- `thingsboard_name`: 1..64 printable ASCII characters
- `manufacturing_state`: 0 (unprovisioned), 1 (provisioned), 2 (quarantined)
- `credential_mode`: 0 (none), 1 (PSK), 2 (mTLS)
- maximum on-storage document size: 2048 bytes

The provisioning secret itself is never stored in a managed document.

## Rejection rules

Documents are rejected (with a distinct error code) when they are:

- missing (`APP_CONFIG_ERR_NOT_FOUND`),
- truncated / malformed JSON / wrong-typed members / unknown or duplicate
  members (`APP_CONFIG_ERR_MALFORMED`),
- oversized at file level (> 2048 bytes) or field level
  (`APP_CONFIG_ERR_BOUNDS`),
- carrying a `schema_version` newer than this implementation
  (`APP_CONFIG_ERR_UNKNOWN_SCHEMA`),
- carrying an older `schema_version` without a registered migration handler
  (`APP_CONFIG_ERR_MIGRATION`).

## Commit (atomic replacement)

`app_config_commit_device()` / `app_config_commit_manufacturing()` run the
normative sequence:

1. validate the document against the bounded schema,
2. write it to `<live>.tmp` (temporary data),
3. flush and close (`osal_close()` is the durability point; the OSAL has no
   separate flush primitive),
4. read the temporary file back and re-validate it,
5. refresh `<live>.good` from the live file — only when the live file still
   validates — validate the staged copy, and atomically replace
   `<live>.good` with it (`osal_rename()`; without atomic rename the
   commit is refused with `APP_CONFIG_ERR_UNSUPPORTED`, so the previous
   last-known-good copy can never be truncated in place),
6. atomically replace the live file with `osal_rename()`.  When the OSAL
   backend does not support rename (`OSAL_ERR_OPERATION_NOT_SUPPORTED` /
   `OSAL_ERR_NOT_IMPLEMENTED`) the commit is refused with
   `APP_CONFIG_ERR_UNSUPPORTED`: a copy-then-remove fallback could leave a
   valid live document truncated if interrupted, so a non-atomic
   replacement path is never used.

Invalid data can therefore never replace valid live configuration: any
failure before step 6 leaves the live file and its last-known-good copy
untouched.

## Recovery (last-known-good)

`app_config_load_device()` / `app_config_load_manufacturing()` read the
live document first.  When the live document is missing or fails
validation, the `.good` copy is validated and — only after it passes —
restored to the live path through the full safe commit path; the caller
then sees `APP_CONFIG_OK_RECOVERED`.  Recovery uses only a validated
backup; when both copies are unusable the error is reported and no
fabricated data is returned.

## Migration (explicit entry points)

- `app_config_register_migration_handler()` registers the hook invoked for
  documents with `0 <= schema_version < 1`; the hook's output must
  re-validate against the current schema before it is trusted,
- `app_config_migrate_stored(kind)` performs an explicit on-storage
  migration: read, migrate, validate, commit through the safe path.

Newer schema versions are always rejected (`APP_CONFIG_ERR_UNKNOWN_SCHEMA`)
— a device must never trust a document written by newer firmware.

## Secret redaction for diagnostics

`app_config_redact()` masks the string values of secret-bearing keys
(`token`, `secret`, `password`, `psk`, `key`, `provisioning`,
case-insensitive) before any captured document is logged; unparsable input
is replaced by a fixed placeholder instead of being echoed.

## Host tests

Two host test targets run the production source under Unity:

- `tests/app_config` target `app_config_tests`: in-memory OSAL filesystem
  double with deterministic fault injection — success, corruption,
  recovery and interrupted writes, plus schema rejection, migration,
  rename-unsupported refusal and redaction,
- `tests/app_config` target `app_config_posix_lfs_tests`: the **real POSIX
  OSAL backend on a temporary file-backed LittleFS volume**
  (`osal_file_impl.c` + `osal_mount_impl.c` + vendored littlefs `lfs.c`),
  exercising the actual `osal_rename()`/`osal_cp()` persistence behavior:
  success, corruption, last-known-good recovery, interrupted/stale
  temporary writes and durability across unmount/remount of the volume.

```sh
cmake -S tests/app_config -B build-app-config-tests
cmake --build build-app-config-tests
ctest --test-dir build-app-config-tests --output-on-failure
```
