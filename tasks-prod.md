# Kitchen LED Controller Production Tasks

These tasks begin after the development checklist in `task.md` has produced a
controller that boots fail-off, verifies TLS, synchronizes desired state, and
passes its development tests. Each task is a standalone prompt for an
implementation model and must be completed in a separate merge request.

Product invariants:

- ESP32-WROOM-32D is the release target; ESP32-S3 and ESP32-C6 are separate
  portability targets.
- A hardware or connection failure must force the external-MOSFET LED output
  electrically off.
- ThingsBoard desired state is authoritative. Do not persist lamp state in
  release 1.
- Do not commit, print, publish, or attach tokens, passwords, private keys,
  provisioning secrets, or certificate payloads.
- Keep reusable capabilities in `platform/hq_platform`; product policy stays
  in this repository.

## [ ] TASK-118 - Add access-token manufacturing fixture flow

Context:
- This is the selected first production authentication method: verified server
  TLS plus one unique ThingsBoard access token per device.
- The fixture must install `/cert/ca.crt`, device identity, and configuration,
  then verify TLS connection, desired-state synchronization, telemetry, RPC,
  and OTA readiness.
- Manufacturing records must map device serial, MAC address, firmware version,
  platform commit, and ThingsBoard device ID. Provisioning secrets must be
  removed after enrollment.

Implement a repeatable, auditable fixture workflow for access-token devices.

Requirements:
- add scripts under `scripts/manufacturing` to allocate serials and create or locate a ThingsBoard device,
- define an explicit manufacturing state record and reject incomplete enrollment,
- install CA, client ID, token, and manufacturing state through the fixture interface,
- verify TLS connection, shared-attribute synchronization, telemetry, RPC, and OTA readiness,
- write an audit record without token or certificate contents,
- remove provisioning-secret material after successful enrollment,
- make reruns idempotent and explicitly identify already-enrolled devices,
- ensure a rejected unit cannot enter normal online state,
- add dry-run or mock tests for success, already-enrolled, verification failure, and secret redaction.

Definition of Done:
- a valid unit completes the documented enrollment verification sequence,
- a rejected or incomplete unit remains fail-off,
- reruns are safe and auditable,
- manufacturing validation passes without exposing credentials.

## [ ] TASK-119 - Integrate OTA with verification and recovery

Context:
- The device uses two OTA slots and existing OSAL OTA APIs. The OSAL owns image
  write/apply mechanics; product code owns policy and reporting.
- TLS protects transport but does not authenticate firmware artifacts. Every
  image requires SHA-256 verification and signed-image verification before use.
- Unsafe initialization, download failures, verification failures, and reboot
  transitions must keep the LED off. Rollouts proceed by internal, canary, then
  production device groups.

Integrate ThingsBoard firmware metadata and chunks with verified, recoverable OTA.

Requirements:
- consume metadata and chunks through the existing ThingsBoard firmware-update module,
- persist transaction state under `/state/ota.json` with bounded, validated fields,
- enforce declared image size and inactive-partition bounds before writing,
- verify SHA-256 and signed-image authenticity before selecting a new boot image,
- abort safely on download, checksum, signature, metadata, or write failure,
- use ESP rollback and explicit post-boot health confirmation,
- prevent downgrade unless an explicit, auditable recovery policy authorizes it,
- publish bounded OTA lifecycle telemetry and redacted errors,
- add tests for success, checksum failure, signature failure, interruption, oversized image, rollback, and confirmation.

Definition of Done:
- an unverified or incomplete image is never selected for boot,
- interruption leaves a bootable confirmed image available,
- rollback and health confirmation work on target hardware,
- OTA tests and the WROOM-32D build pass.

## [ ] TASK-120 - Prototype injected mTLS manufacturing

Context:
- This is optional authentication option B1, not a replacement for the
  access-token production path until a documented go/no-go decision is made.
- The manufacturing station creates a unique private key and certificate, then
  writes `/cert/device.key`, `/cert/device.crt`, and `/cert/ca.crt`.
- The PKI must track certificate serial, device serial, issuance, expiry,
  revocation, and reissuance. Private keys are sensitive fixture material.

Implement and evaluate the injected-certificate manufacturing option.

Requirements:
- add a separate credential mode selected from validated manufacturing configuration,
- configure Mongoose MQTT mTLS using existing CA, client-certificate, and private-key path support,
- constrain credential paths to `/cert` and keep key contents out of logs and telemetry,
- document manufacturing-station key handling, PKI records, expiry, revocation, and reissue process,
- erase certificate, key, CA, identity, and Wi-Fi data during factory reset, then force output off,
- report only non-sensitive certificate metadata such as expiry or fingerprint,
- test valid, missing, wrong, expired where tooling permits, and revoked credentials,
- record a B1 go/no-go decision based on manufacturing complexity and recovery behavior.

Definition of Done:
- unique injected credentials connect with verified mTLS,
- rejected credentials cannot synchronize desired state or enable output,
- factory reset removes all credentials,
- B1 test evidence and decision documentation exist.

## [ ] TASK-121 - Prototype on-device key and CSR enrollment

Context:
- This is optional authentication option B2. The device generates its private
  key locally, sends a CSR containing its assigned identity, and receives only
  its certificate and CA chain.
- Without flash encryption or a secure element, on-device generation does not
  prevent physical extraction from flash. Do not claim production readiness
  without tested renewal and revocation.
- The current platform models provisioning request/response but does not yet
  provide a complete key lifecycle or CSR installation flow.

Implement and evaluate an on-device private-key and CSR enrollment flow.

Requirements:
- select a proven ESP-IDF-compatible crypto and CSR facility; document version, license, and dependency boundary,
- generate private keys using verified device entropy and store them through controlled filesystem/OSAL interfaces,
- create a CSR bound to the assigned serial and device identity,
- define authenticated fixture or enrollment-service request/response handling with strict payload bounds,
- validate issued certificate identity and CA chain before installation,
- handle failed enrollment, interrupted installation, renewal, revocation, and recovery without exposing key material,
- keep output fail-off until valid mTLS enrollment and authoritative state synchronization complete,
- add tests for CSR identity, invalid issuer response, payload limits, renewal, revocation, and failure recovery,
- document physical-access limitations and record a B2 go/no-go decision.

Definition of Done:
- the private key is never transmitted by the enrollment protocol,
- invalid or interrupted enrollment leaves the device recoverable and fail-off,
- renewal and revocation behavior has test evidence,
- B2 decision documentation exists.

## [ ] TASK-122 - Add production hardening profile

Context:
- Development may use plaintext filesystem secrets and a short-lived test CA;
  production must remove development roots/endpoints and reject insecure TLS.
- Secure boot and flash encryption are deferred, but production must document
  the accepted physical-access threat model and preserve a path to enable them.
- Release 1 cannot ship default credentials, private material, debug credential
  paths, plaintext MQTT, or TLS skip verification.

Create a production configuration, audit, and security-signoff gate.

Requirements:
- add an explicit production configuration/profile separate from development defaults,
- remove development endpoints, test CAs, and development credentials from production artifacts,
- reject plaintext MQTT and `skip_verify=true` at configuration validation and runtime,
- disable debug credential paths and define production log levels that redact secrets,
- document physical-access threats, residual plaintext-storage risk, and the flash-encryption/secure-boot decision,
- add an automated release audit for default credentials, private keys, development roots, insecure flags, and generated artifacts,
- add a release checklist requiring explicit security sign-off.

Definition of Done:
- production configuration refuses insecure transport settings,
- automated audit finds no credentials, private keys, or development CA material,
- security documentation and CI checks pass.

## [ ] TASK-123 - Add ESP32-S3 and ESP32-C6 build profiles

Context:
- ESP32-WROOM-32D remains the release target. S3 and C6 are not assumed binary
  compatible, and C6 uses RISC-V.
- Board configuration owns GPIO number, PWM timer/channel, polarity, frequency,
  flash geometry, and partition suitability; product logic must stay OSAL-only.
- Compile success is not hardware support. PWM duty, polarity, and fail-off need
  hardware smoke-test evidence before support is advertised.

Add board-specific portability profiles and CI validation without changing WROOM behavior.

Requirements:
- complete `sdkconfig.defaults.esp32s3` and `sdkconfig.defaults.esp32c6` with board-safe settings,
- make GPIO and PWM capability differences board configuration rather than product-domain conditionals,
- validate flash size and custom partition compatibility per target,
- add compile-only CI jobs for ESP32, ESP32-S3, and ESP32-C6,
- ensure product code uses only OSAL PWM/GPIO abstractions and has no architecture-specific assumptions,
- document required hardware smoke tests: boot/reset off state, 25/50/100 percent duty, polarity, and network disconnect fail-off.

Definition of Done:
- all three targets configure and compile in CI,
- WROOM-32D defaults and behavior remain unchanged,
- S3/C6 support remains labelled compile-only until hardware smoke tests pass.

## [ ] TASK-124 - Prepare the production release candidate

Context:
- Release requires fixed WROOM hardware and flash size, MOSFET electrical and
  thermal validation, fail-off validation for all listed failure paths, verified
  TLS, unique per-device tokens, tested OTA rollback, tested backup restore,
  and complete manufacturing traceability.
- Build metadata and telemetry must record the reviewed `hq_platform` submodule
  commit. Dependencies are frozen before generating release artifacts.

Freeze approved dependencies and assemble reproducible production release evidence.

Requirements:
- pin and record the reviewed `hq_platform` submodule commit in build metadata and release manifest,
- generate an SBOM and manifest identifying firmware version, hardware target, partition table, and platform revision,
- build signed release artifacts reproducibly from a fresh recursive clone,
- document manufacturing release steps and UART/full-flash recovery,
- execute and record the end-to-end test matrix: manufacture, onboarding, TLS, state sync, RPC, telemetry, disconnect fail-off, OTA, rollback, and factory reset,
- record hardware evidence for MOSFET electrical/thermal behavior and WROOM flash-size confirmation,
- record ThingsBoard/PostgreSQL backup-and-restore evidence,
- confirm zero open high-severity defects before candidate tag approval.

Definition of Done:
- a fresh recursive clone produces the documented signed artifact,
- evidence covers every release gate in the production plan,
- release manifest, SBOM, recovery procedure, and formal review package exist,
- the candidate is ready for tag creation after sign-off.