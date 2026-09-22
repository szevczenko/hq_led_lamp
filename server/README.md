# Development LAN ThingsBoard stack (TASK-116)

Reproducible, LAN-only ThingsBoard + PostgreSQL development deployment for
integration testing of the Kitchen LED Controller. Everything lives in this
directory; all runtime material is generated and git-ignored, nothing
private is committed.

```
controller --- Wi-Fi LAN ---> mqtts home-assistance.local:8883 -> ThingsBoard
admin browser       ------> https home-assistance.local         -> Caddy -> ThingsBoard
```

- **MQTT TLS** for devices: `home-assistance.local:8883` (verified TLS,
  development CA).
- **HTTPS** for administration: `home-assistance.local:443` (Caddy reverse
  proxy -> ThingsBoard web UI/API).
- **Plaintext MQTT** (1883) and the raw admin port (9090) are bound to
  localhost only, for development/troubleshooting.
- Data survives container recreation: PostgreSQL is bind-mounted under
  `server/data/postgres`; ThingsBoard data/logs live in the durable named
  volumes `tb-data` / `tb-logs` declared in `compose.yml`.

Production CA custody, hardening, backup evidence, and release sign-off are
documented in `PKI.md` (procedures only — no production material is ever
generated or stored here). This stack is development-only.

## Layout

| Path                    | Purpose                                                        |
|-------------------------|----------------------------------------------------------------|
| `compose.yml`           | Pinned ThingsBoard / PostgreSQL / Caddy services + healthchecks |
| `Caddyfile`             | HTTPS 443 -> ThingsBoard 9090 reverse-proxy config             |
| `.env.example`          | Non-secret environment template (committed)                    |
| `.env`                  | Real environment with generated secrets (**git-ignored**)      |
| `certs/`                | Generated development CA + server key/CSR/certificates (**git-ignored**) |
| `data/`                 | Durable PostgreSQL data directory (**git-ignored**)            |
| `PKI.md`                | Development CA + production CA custody/renewal procedures     |
| `scripts/gen_dev_tls.sh`| One-shot TLS + secrets generation (orchestrates the phases)    |
| `scripts/dev_pki_config.sh` | Shared PKI configuration (sourced by the phase scripts)   |
| `scripts/gen_dev_ca.sh` | Development root CA (ca.key/ca.crt)                           |
| `scripts/gen_server_csr.sh` | Server key + CSR (SAN enforced)                           |
| `scripts/gen_server_cert.sh` | Issue server certificate from the CSR (SAN required)     |
| `scripts/check_pki.py`   | Offline PKI validation (trusted/unknown CA, hostname, profile) |
| `scripts/test_pki.py`    | End-to-end tests for the PKI scripts + validation matrix       |
| `scripts/check_endpoints.py` | Credential-free HTTPS/MQTT-TLS endpoint validation        |

## Prerequisites

- Ubuntu LTS with Docker Engine and the Compose plugin
  (`docker compose version` must work).
- `openssl` (CLI) and `python3` (stdlib only) for setup/validation.
- LAN router with DHCP reservation + local DNS support
  (see below).

## 1. First-time setup

### 1.1 DNS and DHCP (router)

1. Reserve a DHCP address for the Ubuntu server, e.g. `192.168.1.20`.
2. Add a local DNS record:

   ```text
  home-assistance.local -> 192.168.1.20
   ```

  Ensure `home-assistance.local` resolves for the ESP32 and server. If the
  network uses mDNS for `.local`, verify that the ESP32 resolver supports the
  corresponding name before relying on it for MQTT TLS.
3. If your router cannot add local DNS records, run `dnsmasq`/AdGuard Home on
   the Ubuntu server itself and point DHCP clients at it.
4. Keep the stack LAN-only: **do not** configure Internet port forwarding for
   `443` or `8883`.
5. Fallback for quick testing on a single machine: add
  `192.168.1.20 home-assistance.local` to `/etc/hosts` on the server and on
   machines that need the name. The firmware still needs the real name
   resolvable over Wi-Fi DNS — `/etc/hosts` is not enough for devices.

### 1.2 Generate TLS material and secrets

```bash
cd server
./scripts/gen_dev_tls.sh
```

This creates (all git-ignored):

- `server/.env` — environment copied from `.env.example` with a generated
  PostgreSQL password;
- `server/certs/` — development root CA (`ca.crt`, `ca.key`), server private
  key + CSR (`server.key`, `server.csr`, SAN `DNS:home-assistance.local`),
  server certificate (`server.crt`), and the PEM credentials for the
  ThingsBoard MQTT TLS listener (`server.pem`, `server_key.pem`);
- `server/data/postgres` — durable PostgreSQL storage directory.

The generation is split into focused, individually callable phase scripts
(`gen_dev_ca.sh`, `gen_server_csr.sh`, `gen_server_cert.sh`); see
`PKI.md` §2 for the workflow. The signer refuses a CSR whose DNS SAN is not
the documented endpoint, and `python3 server/scripts/check_pki.py` validates
the result offline (trusted-CA success, unknown-CA failure, hostname
mismatch, SAN, key usage, permissions, lifetime, and that no device artifact
contains a private key).

ThingsBoard's own data and logs are Docker named volumes (`tb-data`,
`tb-logs`) — they persist across `docker compose down` and need no host
paths.

The development CA is a throwaway root kept on this host. Devices that
integrate against this stack trust `server/certs/ca.crt` (installed on the
firmware as `/cert/ca.crt` — the **only** PKI file a device ever receives;
private keys never leave this host). Re-generating with `--regenerate`
issues a new CA and invalidates every previously distributed copy of
`ca.crt`.

### 1.3 Trust the development CA (admin browsers / REST tooling)

Browsers and scripts that talk to `https://home-assistance.local` must trust
`server/certs/ca.crt`:

- Browser: import `server/certs/ca.crt` into the OS/browser trust store
  (marked *trusted for identifying websites*).
- Host scripts: add it to the system CA bundle, or pass it explicitly, e.g.
  `curl --cacert server/certs/ca.crt https://home-assistance.local/login`.

### 1.4 Start the stack

```bash
cd server
docker compose up -d
docker compose ps          # watch for "healthy" on all services
```

First boot runs the ThingsBoard database schema install and can take
several minutes. Watch progress with:

```bash
docker compose logs -f thingsboard
```

You are ready when the health checks pass:

```bash
python3 server/scripts/check_endpoints.py --wait 600
```

Expected output (exact status/CONNACK codes may vary by ThingsBoard version):

```text
PASS  HTTPS home-assistance.local:443 -> HTTP/1.1 200 OK ...
PASS  MQTT TLS home-assistance.local:8883 -> CONNACK rc=2 ... (or rc=5 ...)
```

(The exact MQTT CONNACK return code does not matter: any rc from an
anonymous probe proves the TLS listener is up and speaking MQTT — an
unauthenticated device is expected to be rejected, e.g. `rc=2` identifier
rejected or `rc=5` not authorized. Authorized devices connect with their
access token.)

### 1.5 First login

Open `https://home-assistance.local` and log in with the default
system administrator:

- user: `sysadmin@thingsboard.org`
- password: `sysadmin` (change it immediately in a real environment)

The default tenant administrator is `tenant@thingsboard.org` /
`tenant`. See section 4 for creating a tenant API key for the REST test
harness.

## 2. Endpoint summary

| Protocol | Address                        | Scope                  |
|----------|--------------------------------|------------------------|
| HTTPS    | `home-assistance.local:443`    | LAN — admin/web UI/REST via Caddy |
| MQTT TLS | `home-assistance.local:8883`   | LAN — verified device traffic |
| MQTT     | `127.0.0.1:1883` (host)        | loopback only — migration/testing |
| HTTP     | `127.0.0.1:9090` (host)        | loopback only — direct admin/REST |
| Edge RPC | `127.0.0.1:7070` (host)        | loopback only             |

The MQTT TLS listener uses the development server certificate; clients must
verify both the chain (`ca.crt`) and the hostname (`home-assistance.local`),
exactly like the firmware's `mqtt_cfg` (`mqtts://...:8883`, skip-verify
forbidden).

## 3. Startup, shutdown, reset

All commands run from `server/`.

### Start

```bash
docker compose up -d
```

### Stop (keep data)

```bash
docker compose stop          # pause containers
docker compose down          # stop and remove containers, keep data/certs
```

### Reset development environment

Removes containers, named volumes, and all generated runtime material:

```bash
docker compose down -v            # -v also deletes tb-data / tb-logs volumes
rm -rf data certs .env
./scripts/gen_dev_tls.sh           # fresh CA + secrets
docker compose up -d
```

`docker compose down` (without `-v`) alone never touches the data —
PostgreSQL (`server/data/postgres` bind mount) and ThingsBoard state
(`tb-data` / `tb-logs` named volumes) survive container recreation by
design. A plain `docker compose up -d --force-recreate` also keeps all
data.

## 4. Integration-test setup

### 4.1 REST API functional tests (`tests/thingsboard`)

1. Create a tenant-scoped API key in the ThingsBoard UI:
   **Security settings -> API keys** (or **Security -> Advanced ->
   API keys**), `TENANT_ADMIN` scope. Keep it in an environment variable —
   never commit it.
2. Point the existing test harness at this stack (it can use the loopback
   HTTP port, which avoids TLS/CA setup for scripts):

   ```bash
   export TB_API_KEY=<tenant api key>
   export TB_BASE_URL=http://127.0.0.1:9090
   export RUN_TB_FUNCTIONAL_TESTS=1
   python3 -m unittest discover -s tests/thingsboard -v
   ```

  The same harness can run over HTTPS with `TB_BASE_URL=https://home-assistance.local`
   once the development CA is trusted system-wide (see 1.3).
3. To provision a named test device for firmware-style access-token tests:

   ```bash
   export TB_API_KEY=<tenant api key>
   python3 scripts/thingsboard/provision_test_device.py --base-url http://127.0.0.1:9090
   ```

   The access token is stored in the git-ignored
   `scripts/thingsboard/.klc-test-01.token` file.

### 4.2 MQTT TLS device/firmware integration

Devices connect with verified TLS — no plaintext MQTT is exposed on the LAN:

- broker: `mqtts://home-assistance.local:8883`
- trust anchor: `server/certs/ca.crt` (installed as `/cert/ca.crt` on the
  firmware; also the CA this repo's `mqtt_cfg` tests expect)
- device auth: per-device access token (Option A) or X.509 once mTLS arrives

Firmware-side config example (`/config/mqtt.json`, see
`components/mqtt_cfg/README.md`):

```json
{
  "schema_version": 1,
  "hostname": "home-assistance.local",
  "port": 8883,
  "tls_mode": "mqtts",
  "ca_path": "/cert/ca.crt",
  "client_id": "klc-kitchen-01",
  "auth_mode": "access_token",
  "skip_verify": false
}
```

Manual endpoint probes (no credentials needed):

```bash
# MQTT TLS listener
openssl s_client -connect home-assistance.local:8883 \
  -CAfile server/certs/ca.crt -verify_hostname home-assistance.local \
  < /dev/null

# HTTPS listener through Caddy
openssl s_client -connect home-assistance.local:443 \
  -CAfile server/certs/ca.crt -verify_hostname home-assistance.local \
  < /dev/null
```

The automated, credential-free check is:

```bash
python3 server/scripts/check_endpoints.py            # single pass
python3 server/scripts/check_endpoints.py --wait 600 # poll until ready
```

## 5. Health checks and validation

- Compose-level health checks: `docker compose ps` shows `healthy` for
  PostgreSQL (`pg_isready`), ThingsBoard (TCP probe of the admin port), and
  Caddy (admin API probe).
- Offline PKI validation (no server required): `scripts/check_pki.py`
  verifies the generated material — trusted-CA success, unknown-CA failure,
  hostname mismatch, DNS SAN, key usage, permissions, lifetime policy, and
  that no device artifact or git-tracked file contains a private key.
  Expected results are documented in `PKI.md` §7.
- Credential-free endpoint validation: `scripts/check_endpoints.py` verifies
  TLS chain + hostname on both `443` and `8883`, confirms the HTTP login page
  responds, and performs an anonymous MQTT CONNECT/CONNACK exchange. It
  never needs an API key, a device token, or a login.

## 6. Security notes (development stack)

- LAN-only by design: `443` and `8883` are published on the host, no port
  forwarding, no public DNS.
- Plaintext MQTT `1883` and the admin `9090` port are bound to loopback only.
- Everything generated (`server/.env`, `server/certs/`, `server/data/`) is
  git-ignored; these paths never appear in a commit.
- Devices receive **only** the public root `ca.crt` (as `/cert/ca.crt`).
  CA/server private keys stay on this host; `check_pki.py` verifies that no
  device artifact or tracked file contains one.
- The development CA is not for production: firmware must never embed or
  trust development roots in release images. Production CA custody and
  renewal procedures are documented in `PKI.md` §8 — no production material
  is generated by anything in this repository.
- Optional host firewall example (`ufw`):

  ```bash
  sudo ufw allow from 192.168.1.0/24 to any port 443 proto tcp
  sudo ufw allow from 192.168.1.0/24 to any port 8883 proto tcp
  sudo ufw deny 1883/tcp    # loopback-bound anyway; defense in depth
  ```

## 7. Troubleshooting

| Symptom                              | Likely cause / fix                                          |
|--------------------------------------|--------------------------------------------------------------|
| `check_endpoints.py` fails on HTTPS  | Caddy not healthy yet; run with `--wait 600`, or trust `certs/ca.crt` in the client |
| MQTT TLS check fails with cert error | Run `./scripts/gen_dev_tls.sh`; verify `--host` matches the SAN (`home-assistance.local`) |
| ThingsBoard crash-loops: `Unable to find resource: /certs/server_key.pem` | `server_key.pem` lost its container-readable mode (the container cannot read host 600 files over the bind mount). Run `chmod 644 certs/server_key.pem`, or re-run `./scripts/gen_dev_tls.sh --regenerate`, then `docker compose restart thingsboard` |
| Name does not resolve                | Router DNS record or `/etc/hosts` missing (section 1.1)      |
| `docker compose up` stops at postgres password | `.env` missing/incomplete — run `./scripts/gen_dev_tls.sh` |
| First boot hangs on schema install   | Normal for several minutes; watch `docker compose logs -f thingsboard` |
| Devices can't verify TLS             | Install `server/certs/ca.crt` on the device as `/cert/ca.crt`; NTP/time must be correct |

## See also

- `PKI.md` — development CA profile/workflow and the production CA custody
  and renewal procedures (no production material generated here).
- `../KITCHEN_LED_CONTROLLER_PRODUCTION_PLAN.md` — section 9 (TLS/LAN plan),
  section 16.1 (REST functional tests).
- `../components/mqtt_cfg/README.md` — firmware broker/TLS configuration
  (`ca_path: /cert/ca.crt`, `skip_verify: false`).
- `../scripts/thingsboard/README.md` — test-device provisioning and the
  REST functional-test harness.