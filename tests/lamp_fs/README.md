# lamp_fs host tests (TASK-107)

Host unit tests for the filesystem bootstrap in `components/lamp_fs`.

## What is tested

The production component is compiled unmodified against a deterministic
test-only OSAL filesystem double (`osal_fs_mock.c`), which implements the
`osal_mount`/`osal_mkdir`/`osal_unmount` contract with failure injection and
call recording.  Coverage per the TASK-107 definition of done:

- **first mount** — a successful mount creates `/cert`, `/config` and
  `/state` exactly once each in that order, the bootstrap reports mounted,
  and the fail-safe callback is never invoked,
- **existing directories** — directories that already exist
  (`OSAL_ERR_NAME_TAKEN`) are idempotent success on every later boot; the
  fail-safe is not invoked and existing contents are untouched,
- **mount failure** — a failed `osal_mount()` invokes the fail-safe exactly
  once, attempts no directory creation (existing storage preserved) and
  reports `LAMP_FS_ERR_MOUNT`; works safely without a callback too,
- **directory failure** — a failed `osal_mkdir()` invokes the fail-safe,
  stops the creation sequence, keeps the volume mounted (storage preserved
  for the explicit recovery flow) and reports `LAMP_FS_ERR_DIRECTORY`,
- **ordering regression** — directory creation only ever happens after a
  successful mount,
- **lifecycle/argument edges** — unmount when not mounted, NULL/empty
  logical paths, re-init after unmount.

## Running

```sh
cmake -S tests/lamp_fs -B build-lamp-fs-tests
cmake --build build-lamp-fs-tests
ctest --test-dir build-lamp-fs-tests --output-on-failure
```

or run the binary directly:

```sh
./build-lamp-fs-tests/lamp_fs_tests
```

## Layout

- `lamp_fs_test.c` — the test cases (Unity runner).
- `osal_fs_mock.{c,h}` — test-only OSAL filesystem double with failure
  injection and call recording.
- `CMakeLists.txt` — standalone host test project (CTest); reuses the
  vendored Unity framework from `tests/lamp_control/unity`.
