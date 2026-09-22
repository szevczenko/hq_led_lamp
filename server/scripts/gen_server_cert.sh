#!/usr/bin/env bash
# Issue the ThingsBoard server certificate from the CSR with the dev CA.
#
# REQUIRES the documented DNS SAN: the script parses the Subject Alternative
# Name out of server.csr and refuses to sign unless it is exactly
# DNS:<endpoint> (default DNS:home-assistance.local). This is the "require
# the documented DNS SAN" enforcement point of the development PKI.
#
# Produces in server/certs/:
#   server.crt       leaf certificate signed by ca.crt
#   server.pem       leaf + CA chain (ThingsBoard MQTT TLS listener :8883)
#   server_key.pem   server private key copy for the containers (mode 644,
#                    read-only bind-mounted; see server/README.md notes)
#   server.ext       signing profile (audit trail, non-secret)
#   ca.srl           CA serial counter (created by -CAcreateserial)
#
# Prerequisites: gen_dev_ca.sh and gen_server_csr.sh must have run into the
# same directory.
#
# Usage:
#   server/scripts/gen_server_cert.sh [--cert-dir DIR] [--dns-name NAME] [--force]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dev_pki_config.sh
source "$SCRIPT_DIR/dev_pki_config.sh"

CERT_DIR_ARG=""
DNS_NAME_ARG=""
FORCE=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --cert-dir)
            CERT_DIR_ARG="${2:?--cert-dir requires a directory}"
            shift 2
            ;;
        --dns-name)
            DNS_NAME_ARG="${2:?--dns-name requires a name}"
            shift 2
            ;;
        --force) FORCE=1; shift ;;
        *)
            echo "gen_server_cert.sh: unknown option '$1' (see header comment)" >&2
            exit 2
            ;;
    esac
done

if [ -n "$CERT_DIR_ARG" ]; then
    export KLC_CERT_DIR="$CERT_DIR_ARG"
fi
CERT_DIR="$(dev_cert_dir)"
DNS_NAME="$(dev_dns_name "$DNS_NAME_ARG")"
cd "$CERT_DIR"

for f in ca.key ca.crt server.key server.csr; do
    if [ ! -f "$f" ]; then
        echo "gen_server_cert.sh: missing $f - run gen_dev_ca.sh and gen_server_csr.sh first" >&2
        exit 2
    fi
done

if [ "$FORCE" = "1" ]; then
    rm -f server.crt server.pem server_key.pem server.ext
fi

if [ -f server.crt ] && [ -f server.pem ]; then
    echo "Server certificate already exists in $CERT_DIR"
    echo "  (re-run with --force to re-issue from the same CSR)"
    exit 0
fi

# --- Enforce the documented DNS SAN ----------------------------------------
# Parse the subjectAltName extension from the CSR and require exactly
# DNS:<endpoint> with no extra names.
csr_san="$(openssl req -in server.csr -noout -text \
    | sed -n '/Subject Alternative Name/,/^[[:space:]]*[^[:space:]]/p' \
    | grep -oE 'DNS:[^ ,]+' \
    | tr '\n' ' ' \
    | sed 's/ *$//')"
if [ "$csr_san" != "DNS:${DNS_NAME}" ]; then
    echo "gen_server_cert.sh: REFUSING to sign - CSR SAN is '${csr_san:-<none>}'" >&2
    echo "  required SAN: DNS:${DNS_NAME} (the documented endpoint)" >&2
    echo "  regenerate the CSR with gen_server_csr.sh --dns-name ${DNS_NAME}" >&2
    exit 1
fi

# Leaf profile: TLS server authentication only. No CA capability, no
# code/email signing, no alternate names beyond the documented endpoint.
cat > server.ext <<EOF
subjectAltName=DNS:${DNS_NAME}
extendedKeyUsage=serverAuth
keyUsage=digitalSignature,keyEncipherment
basicConstraints=critical,CA:FALSE
EOF

# Never issue a leaf that outlives the retained development CA.  This matters
# during hostname migrations and routine renewal when the CA has less than the
# default leaf lifetime remaining.
ca_not_after="$(openssl x509 -in ca.crt -noout -enddate | cut -d= -f2-)"
ca_not_after_epoch="$(date -d "$ca_not_after" +%s)"
now_epoch="$(date +%s)"
remaining_days=$(( (ca_not_after_epoch - now_epoch) / 86400 - 1 ))
if [ "$remaining_days" -le 0 ]; then
    echo "gen_server_cert.sh: CA certificate is expired or has no safe leaf lifetime remaining" >&2
    exit 1
fi
server_days="$KLC_SERVER_DAYS"
if [ "$server_days" -gt "$remaining_days" ]; then
    server_days="$remaining_days"
fi

openssl x509 -req -in server.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days "$server_days" -"$KLC_HASH" -extfile server.ext

# Convenience artifacts for the compose stack (see server/README.md):
#   server.pem      = leaf + CA chain, served by ThingsBoard MQTT TLS :8883
#   server_key.pem  = server private key copy for the containers
cat server.crt ca.crt > server.pem
cp server.key server_key.pem

# Permissions: private keys 600 on the host; server_key.pem is 644 because
# the ThingsBoard container (unprivileged uid 799) must read it over the
# read-only ./certs bind mount. It is a throwaway development key inside the
# git-ignored directory; see server/README.md for the rationale.
chmod 600 ca.key server.key
chmod 644 server_key.pem
chmod 644 ca.crt server.crt server.pem server.ext

echo "Server certificate issued in $CERT_DIR:"
echo "  server.crt     SAN DNS:${DNS_NAME}, $server_days days (signed by ca.crt)"
echo "  server.pem     leaf + CA chain for ThingsBoard :8883"
echo "  server_key.pem private key copy for the containers (mode 644)"
echo "  server.ext     signing profile (audit)"
echo "Next: docker compose restart thingsboard caddy, then"
echo "      python3 server/scripts/check_pki.py"