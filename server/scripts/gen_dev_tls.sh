#!/usr/bin/env bash
# One-shot setup for the development ThingsBoard stack (TASK-116/TASK-117).
#
# Creates (all git-ignored):
#   server/.env         environment file with generated secrets
#   server/certs/       development root CA + server key/CSR/certificate
#   server/data/postgres  durable PostgreSQL data directory (bind mount)
#
# ThingsBoard's own data and logs live in Docker named volumes declared in
# compose.yml (tb-data / tb-logs) - no host paths, no host-side chown.
#
# Also produces the PEM server credentials for the ThingsBoard MQTT TLS
# listener (port 8883):
#   server/certs/server.pem       server certificate + dev CA chain
#   server/certs/server_key.pem   server private key (mode 0644, container)
#
# The PKI phases are split into dedicated, individually callable scripts
# (see server/PKI.md):
#   server/scripts/gen_dev_ca.sh        development root CA (ca.key/ca.crt)
#   server/scripts/gen_server_csr.sh    server key + CSR (server.key/server.csr)
#   server/scripts/gen_server_cert.sh   sign the CSR (server.crt, SAN required)
# The certificate is REQUIRED to carry SAN DNS:home-assistance.local.
#
# Usage:
#   server/scripts/gen_dev_tls.sh                  # idempotent setup
#   server/scripts/gen_dev_tls.sh --regenerate     # fresh CA + server cert
#
# Idempotent: existing server/.env and server/certs are reused. Use
# --regenerate to issue a fresh CA/server certificate pair - this invalidates
# previously distributed copies of ca.crt, so do it only when you really want
# to re-trust everything.
#
# The output is development-only material: the CA private key lives on this
# host and the certificate is signed for a private home.arpa name. Production
# CA custody and hardening are documented in server/PKI.md - this script
# never generates or touches production material.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dev_pki_config.sh
source "$SCRIPT_DIR/dev_pki_config.sh"

CERT_DIR="$(dev_cert_dir)"
ENV_FILE="$KLC_SERVER_DIR/.env"
ENV_TEMPLATE="$KLC_SERVER_DIR/.env.example"
DATA_DIR="$KLC_SERVER_DIR/data"

REGENERATE=0
if [ "${1:-}" = "--regenerate" ]; then
    REGENERATE=1
fi

mkdir -p "$CERT_DIR" "$DATA_DIR/postgres"

# --- 1. Environment file with generated secrets -----------------------------
if [ ! -f "$ENV_FILE" ]; then
    cp "$ENV_TEMPLATE" "$ENV_FILE"
    chmod 600 "$ENV_FILE"
fi

set_secret() {
    local key="$1"
    if ! grep -qE "^${key}=.+" "$ENV_FILE"; then
        local value
        value="$(openssl rand -hex 16)"
        if grep -q "^${key}=" "$ENV_FILE"; then
            sed -i "s/^${key}=.*/${key}=${value}/" "$ENV_FILE"
        else
            printf '%s=%s\n' "$key" "$value" >> "$ENV_FILE"
        fi
    fi
}
set_secret POSTGRES_PASSWORD

# --- 2. Development PKI (CA -> key/CSR -> certificate) ----------------------
# A missing server.csr forces a new server key, so the certificate must then
# be re-issued from that CSR (otherwise key and certificate would diverge).
NEED_REISSUE=0
if [ "$REGENERATE" = "1" ] || [ ! -f "$CERT_DIR/ca.crt" ]; then
    "$SCRIPT_DIR/gen_dev_ca.sh" --regenerate
else
    "$SCRIPT_DIR/gen_dev_ca.sh"
fi

if [ "$REGENERATE" = "1" ] || [ ! -f "$CERT_DIR/server.csr" ]; then
    "$SCRIPT_DIR/gen_server_csr.sh" --force
    NEED_REISSUE=1
else
    "$SCRIPT_DIR/gen_server_csr.sh"
fi

if [ "$REGENERATE" = "1" ] || [ "$NEED_REISSUE" = "1" ] || [ ! -f "$CERT_DIR/server.crt" ]; then
    "$SCRIPT_DIR/gen_server_cert.sh" --force
else
    "$SCRIPT_DIR/gen_server_cert.sh"
fi

DNS_NAME="$(dev_dns_name)"

echo
echo "Development TLS material ready:"
echo "  CA         $CERT_DIR/ca.crt        (install on devices as /cert/ca.crt)"
echo "  Server     $CERT_DIR/server.crt    (SAN DNS:${DNS_NAME})"
echo "  CSR        $CERT_DIR/server.csr    (kept for audit/renewal)"
echo "  MQTT PEM   $CERT_DIR/server.pem + server_key.pem (ThingsBoard :8883)"
echo "  Secrets    $ENV_FILE               (git-ignored)"
echo
echo "Next steps:"
echo "  python3 server/scripts/check_pki.py          # offline PKI validation"
echo "  cd server && docker compose restart thingsboard caddy"
echo "  python3 server/scripts/check_endpoints.py --wait 600"