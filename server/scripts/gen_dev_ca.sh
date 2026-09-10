#!/usr/bin/env bash
# Generate the development root CA (server/certs/ca.key + ca.crt).
#
# What this is:
#   - A short-lived, self-signed root used ONLY for development. It signs the
#     ThingsBoard server certificate (see gen_server_cert.sh) and its public
#     certificate is what devices trust (installed as /cert/ca.crt).
#   - The CA private key stays on this host; it is never committed
#     (server/certs/ is git-ignored) and never shipped to a device.
#   - Development roots must never appear in production firmware images
#     (see SECURITY.md and server/PKI.md).
#
# Idempotent: if ca.key and ca.crt already exist the script exits without
# touching them. Use --regenerate to create a fresh root - this invalidates
# every previously distributed copy of ca.crt (devices must re-install it)
# and every certificate the old root signed.
#
# Usage:
#   server/scripts/gen_dev_ca.sh [--cert-dir DIR] [--regenerate|--force]
#
# Options:
#   --cert-dir DIR   write to DIR instead of server/certs (for tests)
#   --regenerate     delete and recreate the root CA (alias: --force)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dev_pki_config.sh
source "$SCRIPT_DIR/dev_pki_config.sh"

CERT_DIR_ARG=""
REGENERATE=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --cert-dir)
            CERT_DIR_ARG="${2:?--cert-dir requires a directory}"
            shift 2
            ;;
        --regenerate|--force) REGENERATE=1; shift ;;
        *)
            echo "gen_dev_ca.sh: unknown option '$1' (see header comment)" >&2
            exit 2
            ;;
    esac
done

if [ -n "$CERT_DIR_ARG" ]; then
    export KLC_CERT_DIR="$CERT_DIR_ARG"
fi
CERT_DIR="$(dev_cert_dir)"
mkdir -p "$CERT_DIR"
cd "$CERT_DIR"

if [ "$REGENERATE" = "1" ]; then
    rm -f ca.key ca.crt ca.srl
fi

if [ -f ca.key ] && [ -f ca.crt ]; then
    echo "Development root CA already exists in $CERT_DIR"
    echo "  (re-run with --regenerate to create a fresh root)"
    exit 0
fi

# Development root CA. Key usage is restricted to signing certificates and
# CRLs (keyCertSign, cRLSign); the root is a pure trust anchor and never
# signs end-entity traffic directly.
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:"$KLC_CA_RSA_BITS" -out ca.key
openssl req -x509 -new -nodes -key ca.key -"$KLC_HASH" -days "$KLC_CA_DAYS" \
    -subj "$KLC_CA_SUBJECT" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -addext "subjectKeyIdentifier=hash" \
    -out ca.crt

# ca.key is owner-only; ca.crt is public (it is the device trust anchor, but
# the private signing key must never leave this directory).
chmod 600 ca.key
chmod 644 ca.crt

echo "Development root CA created in $CERT_DIR:"
echo "  ca.key  (PRIVATE - mode 600, never distribute)"
echo "  ca.crt  (public - install on devices as /cert/ca.crt)"
echo "  subject $KLC_CA_SUBJECT, lifetime $KLC_CA_DAYS days, keyUsage keyCertSign,cRLSign"