# Production Security Provisioning

This document describes a production-line design only. Do not run these steps
on the current test board: eFuse configuration is irreversible and can make a
device unrecoverable if keys, images, or manufacturing procedures are wrong.

## Key and image ownership

- Generate Secure Boot v2 signing keys and Flash Encryption keys offline or in
  an HSM. Never store private keys in this repository, firmware images,
  build artifacts, or the device filesystem.
- Define a key custody, backup, rotation, revocation, and per-batch audit
  process before programming production devices.
- Sign every bootloader/application image using the selected ESP-IDF release
  process. Verify that the OTA image format and signing chain are compatible
  with Secure Boot v2 before enabling fuses.
- Keep unsigned development images and test credentials out of the production
  signing path.

## Device provisioning

- Use a unique production provisioning secret per device or controlled batch;
  do not reuse the test provisioning secret.
- Limit access to provisioning credentials and rotate/revoke them after the
  corresponding production run.
- Ensure the identity token and client credentials are stored in a protected
  location. Evaluate NVS encryption or a dedicated encrypted partition for
  identity data, certificates, and manufacturing records.
- Never emit tokens, private keys, passwords, or complete provisioning
  payloads in logs or telemetry.

## Fuse sequence and release gate

1. Freeze and review the ESP-IDF version, partition table, bootloader, signing
   configuration, and recovery procedure for the exact hardware revision.
2. On sacrificial qualification units, enable Secure Boot v2 and Flash
   Encryption in the intended Release mode using production-controlled keys.
3. Verify signed first boot, signed OTA, rollback, power-loss recovery,
   encrypted storage behavior, and the documented RMA/recovery path.
4. Confirm that JTAG and UART download-mode fuse settings match the threat
   model and that an authorized manufacturing recovery route remains viable.
5. Require a second-person review and recorded device/firmware/key-batch
   identifiers before applying irreversible fuses to production units.
6. Keep an auditable record of fuse state and image signing metadata without
   recording secret key material.

## Release checks

- Secure Boot rejects an unsigned or incorrectly signed image.
- OTA accepts only images signed by the authorized production key and
  continues to confirm image health only after the application reaches its
  healthy synchronized state.
- Flash Encryption is enabled with the approved release policy and key
  handling process.
- Credentials and identity data are protected at rest and absent from logs,
  telemetry, crash output, and source control.
- JTAG, UART download, console, and debug logging settings are reviewed for
  the production threat model.
- The recovery and RMA procedure has been exercised on qualification units.