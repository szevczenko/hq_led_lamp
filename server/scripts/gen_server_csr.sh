#!/usr/bin/env bash
# Generate the ThingsBoard server private key and Certificate Signing Request
# (server/key + server/certs/server.csr).
#
# The CSR is created for the documented LAN endpoint only. Its DNS SAN names
# exactly the endpoint resolved by dev_pki_config.sh (default
# home-assistance.local; override with --dns-name or TB_DNS_NAME in
# server/.env). gen_server_cert.sh refuses to sign a CSR that carries any
# other name, so the documented SAN requirement is enforced end to end.
#
# The server private key stays on this host (git-ignored server/certs/) and
# is only ever handed to the ThingsBoard/Caddy containers through the local
# bind mount; devices never receive it.
#
# Idempotent: existing server.key/server.csr are left untouched. Use --force
# to regenerate - the previously issued server.crt is then stale until
# gen_server_cert.sh --force re-issues it.
#
# Usage:
#   server/scripts/gen_server_csr.sh [--cert-dir DIR] [--dns-name NAME] [--force]
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
            echo "gen_server_csr.sh: unknown option '$1' (see header comment)" >&2
            exit 2
            ;;
    esac
done

if [ -n "$CERT_DIR_ARG" ]; then
    export KLC_CERT_DIR="$CERT_DIR_ARG"
fi
CERT_DIR="$(dev_cert_dir)"
DNS_NAME="$(dev_dns_name "$DNS_NAME_ARG")"
mkdir -p "$CERT_DIR"
cd "$CERT_DIR"

if [ "$FORCE" = "1" ]; then
    rm -f server.key server.csr
fi

if [ -f server.key ] && [ -f server.csr ]; then
    echo "Server key/CSR already exist in $CERT_DIR for DNS:${DNS_NAME}"
    echo "  (re-run with --force to regenerate, then gen_server_cert.sh --force)"
    exit 0
fi

# Server key: RSA-2048, owner-only. The CSR embeds the documented DNS SAN so
# the certificate cannot silently be issued for another name.
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:"$KLC_SERVER_RSA_BITS" -out server.key
openssl req -new -key server.key \
    -subj "${KLC_SERVER_SUBJECT_PREFIX}${DNS_NAME}" \
    -addext "subjectAltName=DNS:${DNS_NAME}" \
    -addext "basicConstraints=CA:FALSE" \
    -out server.csr

chmod 600 server.key

echo "Server key/CSR created in $CERT_DIR:"
echo "  server.key  (PRIVATE - mode 600, never distribute)"
echo "  server.csr  (public - must be signed by gen_server_cert.sh)"
echo "  DNS SAN     DNS:${DNS_NAME}"
echo "Next: server/scripts/gen_server_cert.sh"