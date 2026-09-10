#!/usr/bin/env bash
# Generate the development TLS material and secrets for the server/ stack.
#
# Creates (all git-ignored):
#   server/.env         environment file with generated secrets
#   server/certs/       development root CA + server certificate
#   server/data/postgres  durable PostgreSQL data directory (bind mount)
#
# ThingsBoard's own data and logs live in Docker named volumes declared in
# compose.yml (tb-data / tb-logs) - no host paths, no host-side chown.
#
# Also produces the PEM server credentials for the ThingsBoard MQTT TLS
# listener (port 8883):
#   server/certs/server.pem       server certificate + dev CA chain
#   server/certs/server_key.pem   server private key (mode 0600)
#
# Usage:
#   server/scripts/gen_dev_tls.sh                  # idempotent setup
#   server/scripts/gen_dev_tls.sh --regenerate     # rebuild TLS material
#
# Idempotent: existing server/.env and server/certs are reused. Use
# --regenerate to issue a fresh CA/server certificate pair - this invalidates
# previously distributed copies of ca.crt, so do it only when you really want
# to re-trust everything.
#
# The output is development-only material: the CA private key lives on this
# host and the certificate is signed for a private home.arpa name. Production
# CA custody and hardening are outside this task (see tasks-prod.md).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$(dirname "$SCRIPT_DIR")"
CERT_DIR="$SERVER_DIR/certs"
ENV_FILE="$SERVER_DIR/.env"
ENV_TEMPLATE="$SERVER_DIR/.env.example"
DATA_DIR="$SERVER_DIR/data"

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

# --- 2. Development root CA and server certificate --------------------------
if [ "$REGENERATE" = "1" ] || [ ! -f "$CERT_DIR/ca.crt" ] || [ ! -f "$CERT_DIR/server.crt" ]; then
    TB_DNS_NAME="$(grep -E '^TB_DNS_NAME=' "$ENV_FILE" | tail -n 1 | cut -d= -f2- || true)"
    if [ -z "$TB_DNS_NAME" ]; then
        TB_DNS_NAME="thingsboard.home.arpa"
    fi

    cd "$CERT_DIR"
    rm -f ca.key ca.crt ca.srl server.key server.csr server.crt server.ext \
          server.pem server_key.pem

    # Development root CA (private key stays on this host, git-ignored).
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:4096 -out ca.key
    openssl req -x509 -new -nodes -key ca.key -sha256 -days 825 \
        -subj "/O=Kitchen LED Controller Dev/CN=hq-dev-tb-ca" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -addext "subjectKeyIdentifier=hash" \
        -out ca.crt

    # Server certificate with the LAN DNS name in the SAN.
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out server.key
    openssl req -new -key server.key \
        -subj "/O=Kitchen LED Controller Dev/CN=${TB_DNS_NAME}" \
        -out server.csr
    cat > server.ext <<EOF
subjectAltName=DNS:${TB_DNS_NAME}
extendedKeyUsage=serverAuth
keyUsage=digitalSignature,keyEncipherment
basicConstraints=CA:FALSE
EOF
    openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
        -out server.crt -days 825 -sha256 -extfile server.ext

    # PEM server credentials for the ThingsBoard MQTT TLS listener (chain).
    cat server.crt ca.crt > server.pem
    cp server.key server_key.pem

    # ca.key / server.key are only read on the host (Caddy container runs as
    # root), so they stay 600. server_key.pem must be readable by the
    # ThingsBoard container, which runs as the unprivileged `thingsboard`
    # user (uid 799) and reaches the key through the read-only ./certs bind
    # mount - bind mounts keep host permissions, so a 600 root/owner-only
    # file would crash the MQTT TLS listener with
    # "Unable to find resource: /certs/server_key.pem". This is a throwaway
    # development key in a git-ignored directory, so 644 (world-readable on
    # this host only) is acceptable here.
    chmod 600 ca.key server.key
    chmod 644 server_key.pem
    chmod 644 ca.crt server.crt server.pem
    rm -f server.csr server.ext
fi

echo "Development TLS material ready:"
echo "  CA         $CERT_DIR/ca.crt        (install on devices as /cert/ca.crt)"
echo "  Server     $CERT_DIR/server.crt    (SAN DNS:${TB_DNS_NAME:-thingsboard.home.arpa})"
echo "  MQTT PEM   $CERT_DIR/server.pem + server_key.pem (ThingsBoard :8883)"
echo "  Secrets    $ENV_FILE               (git-ignored)"
echo
echo "Next steps:"
echo "  cd server && docker compose up -d"
echo "  python3 server/scripts/check_endpoints.py --wait 600"