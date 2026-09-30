#!/usr/bin/env bash
# Shared configuration for the development PKI scripts (TASK-117).
#
# Sourced by:
#   server/scripts/gen_dev_ca.sh        (development root CA)
#   server/scripts/gen_server_csr.sh    (server key + CSR)
#   server/scripts/gen_server_cert.sh   (server certificate)
#   server/scripts/gen_dev_tls.sh       (one-shot stack setup)
#
# This file contains no secrets. All generated material (private keys,
# certificates, CSRs) is written by the phase scripts into the git-ignored
# directory reported by dev_cert_dir() and must never be committed.
# Production CA custody is documented in server/PKI.md - nothing in this
# tree ever generates or stores production material.
set -euo pipefail

KLC_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KLC_SERVER_DIR="$(dirname "$KLC_SCRIPT_DIR")"

# --- Documented endpoint identity -------------------------------------------
# The LAN name the development server certificate MUST carry in its DNS SAN.
# Public ACME cannot validate private LAN names, so the development CA signs
# for this name directly.
KLC_DEV_DNS_NAME="home-assistance.local"

# --- Certificate profile (documented in server/PKI.md) ----------------------
KLC_CA_RSA_BITS=4096          # dev root CA key size
KLC_SERVER_RSA_BITS=2048      # dev server key size
KLC_HASH=sha256
KLC_CA_DAYS=825               # dev root CA lifetime (days, ~27 months)
KLC_SERVER_DAYS=825           # dev server certificate lifetime (days)
KLC_CA_SUBJECT="/O=Kitchen LED Controller Dev/CN=hq-dev-tb-ca"
KLC_SERVER_SUBJECT_PREFIX="/O=Kitchen LED Controller Dev/CN="

# --- Output directory --------------------------------------------------------
# Defaults to the git-ignored server/certs/. Override with KLC_CERT_DIR (the
# test harness uses a scratch directory so tests never touch real material).
dev_cert_dir() {
    printf '%s' "${KLC_CERT_DIR:-$KLC_SERVER_DIR/certs}"
}

# --- Endpoint name resolution -------------------------------------------------
# Resolution order:
#   1. an explicit name passed to the phase script,
#   2. TB_DNS_NAME from server/.env,
#   3. the documented default (home-assistance.local).
# The server certificate is REQUIRED to carry exactly this name in its DNS SAN.
dev_dns_name() {
    local explicit="${1:-}"
    if [ -n "$explicit" ]; then
        printf '%s' "$explicit"
        return 0
    fi
    if [ -f "$KLC_SERVER_DIR/.env" ]; then
        local name
        name="$(grep -E '^TB_DNS_NAME=' "$KLC_SERVER_DIR/.env" | tail -n 1 | cut -d= -f2- || true)"
        if [ -n "$name" ]; then
            printf '%s' "$name"
            return 0
        fi
    fi
    printf '%s' "$KLC_DEV_DNS_NAME"
}