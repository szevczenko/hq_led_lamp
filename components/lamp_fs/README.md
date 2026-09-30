# lamp_fs — LittleFS filesystem bootstrap (TASK-107)

Boot-time filesystem bring-up for the kitchen LED controller: mount the
LittleFS partition through the OSAL, create the application directory layout
idempotently, and fail safely (output off) when storage is unusable.

## What it does

On `lamp_fs_init()`:

1. Mounts the LittleFS partition `storage` at `/littlefs` through the OSAL
   (`osal_mount()`, `osal_dir.h`/`osal_mount.h` contract).  No format, no
   erase — ever — on the boot path.
2. Creates the three application directories idempotently.  A directory
   that already exists (`OSAL_ERR_NAME_TAKEN`) is success:

   | Logical path | Contents                                                        |
   |--------------|-----------------------------------------------------------------|
   | `/cert`      | `ca.crt`, optional `device.crt`, optional `device.key` (secret) |
   | `/config`    | `wifi.json`, `mqtt.json`, `device.json`, `manufacturing.json`   |
   | `/state`     | `ota.json` and other runtime state                              |

3. On any failure (mount error or directory error) the configured fail-safe
   callback runs exactly once and the failure status is returned.  The
   product layer (`main/app_main.c`) wires that callback to
   `lamp_control_force_inactive()`, so the lamp output is off while storage
   is unusable, and skips configuration loading.
4. A directory-creation failure never leaves a hidden mount behind: before
   the error is returned the volume is unmounted through `osal_unmount()`,
   so `lamp_fs_is_mounted()`, the OSAL backend state and a later retry stay
   consistent.  If that unmount itself fails, the volume remains mounted in
   the backend, `lamp_fs_is_mounted()` keeps reporting the true state, and
   `LAMP_FS_ERR_UNMOUNT` is reported instead.

The component is hardware-independent: it speaks only the portable OSAL
filesystem API and never references ESP-IDF VFS, `esp_littlefs` or flash
partition symbols, so it is host-testable as-is.

## Partition layout

Target: ESP32-WROOM-32D, 4 MiB flash.  `partitions.csv` (validated at
configure time by `cmake/klc_validate.cmake`):

| Name     | Type | SubType  | Offset   | Size    | Purpose                     |
|----------|------|----------|----------|---------|-----------------------------|
| nvs      | data | nvs      | 0x9000   | 0x6000  | Wi-Fi credentials, cal data |
| phy_init | data | phy      | 0xf000   | 0x1000  | PHY init data               |
| otadata  | data | ota      | 0x10000  | 0x2000  | active OTA slot selection   |
| coredump | data | coredump | 0x12000  | 0xE000  | Coredump/padding            |
| ota_0    | app  | ota_0    | 0x20000  | 0x1C0000| OTA slot 0 (1792 KiB)       |
| ota_1    | app  | ota_1    | 0x1E0000 | 0x1C0000| OTA slot 1 (1792 KiB)       |
| storage  | data | littlefs | 0x3A0000 | 0x60000 | LittleFS volume (384 KiB)   |

Sizing rationale:

- The measured application image is far below 1 MiB, so 1792 KiB per OTA
  slot leaves a large safety margin for future firmware growth (ThingsBoard
  stack, TLS, Wi-Fi provisioning).
- ESP-IDF requires app partitions to be 64 KiB aligned, and otadata ends at
  0x12000, so a coredump partition fills 0x12000-0x20000, keeping the table
  gap-free while both OTA slots stay 64 KiB aligned.
- The LittleFS partition gets the whole remaining tail of the 4 MiB flash
  (384 KiB) — comfortably above the 256 KiB minimum from the plan
  (section 7.1) — holding certificates, configuration, OTA state and
  diagnostics.
- The 4 MiB WROOM-32D flash SKU fills exactly with no gaps.  The configure
  step re-checks offsets, alignment, overlaps and the flash-size bound and
  fails the build on any violation (including a flash-size/CSV mismatch, so
  an SKU change cannot go unnoticed).

## Formatting policy (normative)

The boot path NEVER formats the filesystem.  Formatting is destructive to
credential storage (device keys, certificates, manufacturing state).

**Formatting is permitted only when all of the following hold:**

- the operation is an explicit, operator-triggered manufacturing or
  provisioning action (factory reset, re-provisioning, first-time image
  bring-up on known-blank hardware in the factory flow),
- it is never an automatic reaction to a boot error or a missing/corrupt
  filesystem,
- the operation runs through the explicit OSAL format entry points
  (`osal_mkfs()`/`osal_rmfs()`) in the manufacturing flow — never through
  the bootstrap — and forces the lamp output off afterwards.

**Safe failure is required when:**

- `osal_mount()` fails at boot (missing or corrupt filesystem): existing
  storage must be preserved untouched; the fail-safe runs, the error is
  logged, and boot degrades (output off, configuration not loaded).
- directory creation fails: already-created directories and all existing
  file contents are preserved; fail-safe + reported error, as above.

A corrupted or missing filesystem is therefore a degraded, safe boot; the
recovery path is the explicit manufacturing/provisioning flow, never a
silent reformat.

## Layout

- `include/lamp_fs.h` — normative public API and contract documentation.
- `lamp_fs.c` — implementation.
- `CMakeLists.txt` — ESP-IDF component registration (`REQUIRES osal`).

## Host tests

`tests/lamp_fs` compiles the production source against an in-memory OSAL
filesystem double with failure injection and call recording:

- first mount: `/cert`, `/config`, `/state` created exactly once each;
  success never runs the fail-safe,
- existing directories (`OSAL_ERR_NAME_TAKEN`): idempotent success, no
  fail-safe, contents untouched,
- mount failure: fail-safe runs exactly once, no directory touched
  (existing storage preserved), no format attempted,
- directory failure: fail-safe runs, sequence stops, and the volume is
  unmounted before the error is returned — no residual mount, backend and
  `lamp_fs_is_mounted()` agree, a retry is clean,
- directory failure with a failing unmount: `LAMP_FS_ERR_UNMOUNT` is
  reported and `lamp_fs_is_mounted()` stays consistent with the still-
  mounted backend,
- no format: `osal_mkfs()`/`osal_rmfs()` counters stay zero across every
  boot scenario (first mount, mount failure, directory failure),
- ordering regression: directory creation only ever follows a successful
  mount,
- lifecycle/argument edge cases (unmount when not mounted, NULL/empty path,
  re-init after unmount, retry after a failed bootstrap).

```sh
cmake -S tests/lamp_fs -B build-lamp-fs-tests
cmake --build build-lamp-fs-tests
ctest --test-dir build-lamp-fs-tests --output-on-failure
```
