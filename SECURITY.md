# Security Policy

## Supported versions

Only the latest tagged release receives security fixes.

## Reporting a vulnerability

Report security issues privately to the repository maintainers.
Do not open a public issue for a vulnerability.

## Known constraints

- Wi-Fi credentials and MQTT configuration are stored in plaintext on
  the device filesystem. Physical access to flash is a known risk.
  Flash encryption will be evaluated before production deployment.
  See `KITCHEN_LED_CONTROLLER_PRODUCTION_PLAN.md` §3.2 and §22.

- Development CA roots must never appear in production firmware images.

- Private keys, access tokens, and provisioning secrets must never be
  logged, committed to version control, or included in CI artifacts.
