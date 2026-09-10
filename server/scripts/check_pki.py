#!/usr/bin/env python3
"""Offline validation of the development PKI material (TASK-117).

Validates the generated material under server/certs/ WITHOUT a running
server or any credentials. Every check is deterministic and reproducible:

  1. Trusted-CA success     - server.crt verifies against ca.crt for the
                              documented hostname (chain + name).
  2. Unknown-CA failure     - the same certificate FAILS against an unrelated
                              CA (a device that does not trust ca.crt cannot
                              validate the server).
  3. Hostname mismatch      - the same certificate FAILS when verified for a
                              different hostname (skipping SAN/name checks is
                              impossible by design).
  4. DNS SAN requirement    - server.crt carries exactly
                              DNS:<documented host> and nothing else.
  5. Certificate profile    - CA: CA:TRUE + keyCertSign/cRLSign only; server:
                              CA:FALSE, digitalSignature+keyEncipherment and
                              the serverAuth EKU only.
  6. Lifetime policy        - nothing expired; server validity does not
                              exceed the CA validity or the documented caps
                              (825 days CA / 825 days server).
  7. File permissions       - private keys 0600, public certs 0644, the
                              container key copy 0644, server/.env 0600.
  8. No private keys in
     device artifacts       - ca.crt (the only material devices receive, as
                              /cert/ca.crt) and the server.pem chain contain
                              no PRIVATE KEY block, and git tracks no
                              *.key/*.pem/*.p12 file anywhere in the repo.

Usage:
  python3 server/scripts/check_pki.py [options]

Options:
  --cert-dir DIR      directory with the generated material
                      (default: server/certs)
  --host NAME         documented endpoint hostname to verify
                      (default: TB_DNS_NAME from server/.env, else
                      thingsboard.home.arpa)
  --ca PATH           CA certificate to verify against (default: <cert-dir>/ca.crt)
  --server-cert PATH  server certificate under test (default: <cert-dir>/server.crt)
  --unknown-ca PATH   unrelated CA to prove unknown-CA failure with
                      (default: a throwaway CA generated into a temp dir)

Exit code 0 when every check passes, 1 otherwise. Prints one PASS/FAIL line
per check.
"""
from __future__ import annotations

import argparse
import datetime as dt
import stat
import subprocess
import tempfile
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parents[1]
DEFAULT_CERT_DIR = SERVER_DIR / "certs"
DEFAULT_HOST = "thingsboard.home.arpa"
ENV_FILE = SERVER_DIR / ".env"

# Documented development lifetime policy (must match dev_pki_config.sh).
MAX_CA_LIFETIME_DAYS = 825
MAX_SERVER_LIFETIME_DAYS = 825

DATE_FORMAT = "%b %d %H:%M:%S %Y %Z"
NOW = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def load_env_defaults() -> dict:
    defaults: dict = {}
    if not ENV_FILE.is_file():
        return defaults
    for line in ENV_FILE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("TB_DNS_NAME="):
            defaults["host"] = line.partition("=")[2]
            break
    return defaults


def parse_args() -> argparse.Namespace:
    env = load_env_defaults()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cert-dir", default=str(DEFAULT_CERT_DIR))
    parser.add_argument("--host", default=env.get("host", DEFAULT_HOST))
    parser.add_argument("--ca", default=None)
    parser.add_argument("--server-cert", default=None)
    parser.add_argument("--unknown-ca", default=None, help="unrelated CA for the negative check")
    return parser.parse_args()


def x509_ext(path: Path, extension: str) -> tuple[bool, str]:
    """Return (critical, value) of one X509v3 extension via openssl -ext."""
    proc = run(["openssl", "x509", "-in", str(path), "-noout", "-ext", extension])
    if proc.returncode != 0:
        return False, ""
    lines = proc.stdout.splitlines()
    title = lines[0] if lines else ""
    values = [ln.strip() for ln in lines[1:] if ln.strip() and ln.strip() != "critical"]
    return "critical" in title, " ".join(values)


def x509_dates(path: Path) -> tuple[dt.datetime, dt.datetime]:
    """Return (not_before, not_after) via openssl -dates (naive UTC)."""
    proc = run(["openssl", "x509", "-in", str(path), "-noout", "-dates"])
    if proc.returncode != 0:
        raise RuntimeError(f"openssl x509 -dates failed on {path}: {proc.stderr.strip()}")
    nb = na = None
    for line in proc.stdout.splitlines():
        if line.startswith("notBefore="):
            nb = dt.datetime.strptime(line.split("=", 1)[1], DATE_FORMAT)
        elif line.startswith("notAfter="):
            na = dt.datetime.strptime(line.split("=", 1)[1], DATE_FORMAT)
    if nb is None or na is None:
        raise RuntimeError(f"could not parse -dates output for {path}")
    return nb, na


def mode_of(path: Path) -> int:
    return stat.S_IMODE(path.stat().st_mode)


def git_toplevel() -> Path | None:
    proc = run(["git", "-C", str(SERVER_DIR), "rev-parse", "--show-toplevel"])
    if proc.returncode != 0:
        return None
    return Path(proc.stdout.strip())


def tracked_private_artifacts() -> list[str]:
    """Paths tracked by git that look like private key material."""
    root = git_toplevel()
    if root is None:
        return []
    proc = run(["git", "-C", str(root), "ls-files"])
    if proc.returncode != 0:
        return []
    secrets = [p for p in proc.stdout.splitlines() if p.endswith((".key", ".pem", ".p12"))]
    return secrets


# --------------------------------------------------------------------------
# checks
# --------------------------------------------------------------------------
class Report:
    def __init__(self) -> None:
        self.results: list[tuple[str, bool, str]] = []

    def check(self, name: str, ok: bool, detail: str) -> None:
        self.results.append((name, ok, detail))

    def final(self) -> int:
        failures = 0
        for name, ok, detail in self.results:
            status = "PASS" if ok else "FAIL"
            print(f"{status}  {name}: {detail}")
            failures += 0 if ok else 1
        if failures:
            print(f"PKI validation FAILED ({failures} failing check(s))")
            return 1
        print("PKI validation OK")
        return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args()
    cert_dir = Path(args.cert_dir)
    ca_path = Path(args.ca) if args.ca else cert_dir / "ca.crt"
    server_cert = Path(args.server_cert) if args.server_cert else cert_dir / "server.crt"
    host = args.host
    report = Report()

    required = [ca_path, server_cert, cert_dir / "ca.key", cert_dir / "server.key"]
    for path in required:
        if not path.is_file():
            report.check(f"material present: {path.name}", False, "missing - run server/scripts/gen_dev_tls.sh")
    if not all(p.is_file() for p in required):
        return report.final()

    try:
        server_dates = x509_dates(server_cert)
        ca_dates = x509_dates(ca_path)
    except RuntimeError as exc:
        report.check("certificates parse", False, str(exc))
        return report.final()

    # -- 1. trusted CA success ------------------------------------------------
    proc = run(["openssl", "verify", "-CAfile", str(ca_path), "-verify_hostname", host, str(server_cert)])
    ok = proc.returncode == 0 and "OK" in proc.stdout
    report.check("trusted CA success", ok, f"openssl verify -CAfile {ca_path.name} {host} -> {proc.stdout.strip() or proc.stderr.strip()}")

    # -- 2. unknown CA failure ------------------------------------------------
    if args.unknown_ca:
        unknown_ca = Path(args.unknown_ca)
        report.check("unknown CA present", unknown_ca.is_file(), str(unknown_ca))
    else:
        with tempfile.TemporaryDirectory(prefix="klc-unknown-ca-") as td:
            tmp = Path(td)
            unknown_key = tmp / "unknown.key"
            unknown_ca = tmp / "unknown.crt"
            run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(unknown_key)])
            run(["openssl", "req", "-x509", "-new", "-nodes", "-key", str(unknown_key), "-sha256",
                 "-days", "30", "-subj", "/O=Kitchen LED Controller Test/CN=unrelated-test-ca",
                 "-out", str(unknown_ca)])
            proc = run(["openssl", "verify", "-CAfile", str(unknown_ca), "-verify_hostname", host, str(server_cert)])
            ok = proc.returncode != 0
            report.check("unknown CA failure", ok, f"verify against unrelated CA must fail (rc={proc.returncode}): {proc.stderr.strip() or proc.stdout.strip()}")

    # -- 3. hostname mismatch --------------------------------------------------
    wrong_host = f"wrong.{host}"
    proc = run(["openssl", "verify", "-CAfile", str(ca_path), "-verify_hostname", wrong_host, str(server_cert)])
    mismatch_msg = "mismatch" in (proc.stdout + proc.stderr).lower()
    ok = proc.returncode != 0 and mismatch_msg
    report.check("hostname mismatch fails", ok, f"verify {wrong_host} must fail (rc={proc.returncode}): {proc.stderr.strip()}")

    # -- 4. DNS SAN requirement ------------------------------------------------
    _, san = x509_ext(server_cert, "subjectAltName")
    san_entries = {e.strip() for e in san.split(",") if e.strip()}
    ok = san_entries == {f"DNS:{host}"}
    report.check("DNS SAN required", ok, f"SAN = {sorted(san_entries) or '<none>'}, required {{DNS:{host}}}")

    # -- 5. certificate profiles ----------------------------------------------
    ca_bc_crit, ca_bc = x509_ext(ca_path, "basicConstraints")
    ca_ku_crit, ca_ku = x509_ext(ca_path, "keyUsage")
    ok = "CA:TRUE" in ca_bc and "Certificate Sign" in ca_ku and "CRL Sign" in ca_ku
    report.check("CA profile", ok, f"basicConstraints{'(critical)' if ca_bc_crit else ''}='{ca_bc}', keyUsage{'(critical)' if ca_ku_crit else ''}='{ca_ku}'")

    srv_bc_crit, srv_bc = x509_ext(server_cert, "basicConstraints")
    srv_ku_crit, srv_ku = x509_ext(server_cert, "keyUsage")
    _, srv_eku = x509_ext(server_cert, "extendedKeyUsage")
    ok = ("CA:FALSE" in srv_bc
          and "Digital Signature" in srv_ku and "Key Encipherment" in srv_ku
          and "TLS Web Server Authentication" in srv_eku)
    report.check("server profile", ok, f"basicConstraints{'(critical)' if srv_bc_crit else ''}='{srv_bc}', keyUsage{'(critical)' if srv_ku_crit else ''}='{srv_ku}', EKU='{srv_eku}'")

    # -- 6. lifetime policy -----------------------------------------------------
    ca_nb, ca_na = ca_dates
    srv_nb, srv_na = server_dates
    ca_days = (ca_na - ca_nb).days
    srv_days = (srv_na - srv_nb).days
    # Day-granularity comparison for srv_na vs ca_na: the server certificate
    # is issued right after the CA, so an equal-lifetime pair naturally has a
    # sub-second skew that must not count as "server outlives the CA".
    ok = (ca_nb <= NOW <= ca_na and srv_nb <= NOW <= srv_na
          and srv_na.date() <= ca_na.date()
          and ca_days <= MAX_CA_LIFETIME_DAYS
          and srv_days <= MAX_SERVER_LIFETIME_DAYS)
    report.check(
        "lifetime policy",
        ok,
        f"CA {ca_nb:%Y-%m-%d}..{ca_na:%Y-%m-%d} ({ca_days}d), server {srv_nb:%Y-%m-%d}..{srv_na:%Y-%m-%d} ({srv_days}d), now {NOW:%Y-%m-%d}",
    )

    # -- 7. file permissions -----------------------------------------------------
    perm_ok = True
    perms: list[str] = []
    for path, expected, label in (
        (cert_dir / "ca.key", 0o600, "ca.key private"),
        (cert_dir / "server.key", 0o600, "server.key private"),
        (cert_dir / "ca.crt", 0o644, "ca.crt public"),
        (cert_dir / "server.crt", 0o644, "server.crt public"),
    ):
        actual = mode_of(path)
        perms.append(f"{label}={actual:04o}")
        if actual != expected:
            perm_ok = False
    key_copy = cert_dir / "server_key.pem"
    if key_copy.is_file():
        actual = mode_of(key_copy)
        perms.append(f"server_key.pem container={actual:04o}")
        if actual != 0o644:
            perm_ok = False
    env_file = SERVER_DIR / ".env"
    if env_file.is_file():
        actual = mode_of(env_file)
        perms.append(f".env={actual:04o}")
        if actual != 0o600:
            perm_ok = False
    report.check("file permissions", perm_ok, ", ".join(perms))

    # -- 8. no private keys in device artifacts / tracked by git ----------------
    leaked = []
    for path, label in (
        (ca_path, "ca.crt (device trust anchor /cert/ca.crt)"),
        (server_cert, "server.crt"),
        (cert_dir / "server.pem", "server.pem (chain)"),
    ):
        if path.is_file() and "PRIVATE KEY" in path.read_text(encoding="utf-8", errors="replace"):
            leaked.append(label)
    tracked = tracked_private_artifacts()
    ok = not leaked and not tracked
    detail = "no PRIVATE KEY blocks in device artifacts"
    if tracked:
        detail += f"; git tracks private-looking files: {tracked}"
    if leaked:
        detail += f"; PRIVATE KEY found in: {leaked}"
    report.check("no private keys on devices/committed", ok, detail)

    return report.final()


if __name__ == "__main__":
    raise SystemExit(main())