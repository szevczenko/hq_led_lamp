#!/usr/bin/env python3
"""End-to-end tests for the development PKI (TASK-117).

Runs the real phase scripts (gen_dev_ca.sh, gen_server_csr.sh,
gen_server_cert.sh) against a scratch directory, then validates the material
with check_pki.py. The tests assert:

  * the documented workflow generates the expected (git-ignored) material
    with the documented DNS SAN and file permissions;
  * trusted-CA success, unknown-CA failure and hostname-mismatch checks all
    behave as documented (expected TLS success/failure matrix);
  * a CSR carrying a different DNS SAN is refused by gen_server_cert.sh;
  * check_pki.py FAILS when the certificate is verified for a host that is
    not in the SAN;
  * every generated key/certificate/CSR path under server/certs/ is ignored
    by git, and no private-key material is ever tracked by git.

Requires: bash, openssl, git, python >= 3.10. Run from the repository root:

  python3 server/scripts/test_pki.py

or, to see per-test names:

  python3 -m unittest discover -s server/scripts -p 'test_*.py' -v
"""
from __future__ import annotations

import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "server" / "scripts"
DEFAULT_HOST = "home-assistance.local"
WRONG_HOST = "kdc-other.example.com"

GEN_CA = SCRIPTS / "gen_dev_ca.sh"
GEN_CSR = SCRIPTS / "gen_server_csr.sh"
GEN_CERT = SCRIPTS / "gen_server_cert.sh"
CHECK_PKI = SCRIPTS / "check_pki.py"


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def generate_material(cert_dir: Path, host: str = DEFAULT_HOST) -> None:
    for script, args in (
        (GEN_CA, ["--cert-dir", str(cert_dir)]),
        (GEN_CSR, ["--cert-dir", str(cert_dir), "--dns-name", host]),
        (GEN_CERT, ["--cert-dir", str(cert_dir), "--dns-name", host]),
    ):
        proc = run(["bash", str(script), *args])
        if proc.returncode != 0:
            raise AssertionError(
                f"{script.name} failed (rc={proc.returncode}):\n{proc.stdout}\n{proc.stderr}"
            )


def mode_of(path: Path) -> int:
    return stat.S_IMODE(path.stat().st_mode)


class PkiScratch(unittest.TestCase):
    """Shared scratch workspace generated once for the class."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = Path(tempfile.mkdtemp(prefix="klc-pki-test-"))
        cls.certs = cls.tmp / "certs"
        cls.certs.mkdir()
        generate_material(cls.certs)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)


class TestGenerationAndValidation(PkiScratch):
    def test_material_generated_with_documented_san(self) -> None:
        for name in ("ca.crt", "ca.key", "server.crt", "server.key",
                     "server.csr", "server.pem", "server_key.pem"):
            self.assertTrue((self.certs / name).is_file(), f"missing {name}")

        san = run(["openssl", "x509", "-in", str(self.certs / "server.crt"),
                   "-noout", "-ext", "subjectAltName"])
        self.assertIn(f"DNS:{DEFAULT_HOST}", san.stdout)

        proc = run(["openssl", "verify", "-CAfile", str(self.certs / "ca.crt"),
                    "-verify_hostname", DEFAULT_HOST, str(self.certs / "server.crt")])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_permissions(self) -> None:
        self.assertEqual(mode_of(self.certs / "ca.key"), 0o600)
        self.assertEqual(mode_of(self.certs / "server.key"), 0o600)
        self.assertEqual(mode_of(self.certs / "ca.crt"), 0o644)
        self.assertEqual(mode_of(self.certs / "server.crt"), 0o644)
        self.assertEqual(mode_of(self.certs / "server_key.pem"), 0o644)

    def test_device_trust_anchor_has_no_private_key(self) -> None:
        ca_crt = (self.certs / "ca.crt").read_text(encoding="utf-8")
        self.assertIn("BEGIN CERTIFICATE", ca_crt)
        self.assertNotIn("PRIVATE KEY", ca_crt)
        server_pem = (self.certs / "server.pem").read_text(encoding="utf-8")
        self.assertNotIn("PRIVATE KEY", server_pem)

    def test_validation_success_and_failure_matrix(self) -> None:
        proc = run([sys.executable, str(CHECK_PKI),
                    "--cert-dir", str(self.certs), "--host", DEFAULT_HOST])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PKI validation OK", proc.stdout)
        for expected in (
            "PASS  trusted CA success",
            "PASS  unknown CA failure",
            "PASS  hostname mismatch fails",
            "PASS  DNS SAN required",
        ):
            self.assertIn(expected, proc.stdout)

        # The certificate verified for the WRONG hostname must FAIL overall.
        proc = run([sys.executable, str(CHECK_PKI),
                    "--cert-dir", str(self.certs), "--host", WRONG_HOST])
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("FAIL  trusted CA success", proc.stdout)


class TestSanEnforcement(PkiScratch):
    def test_csr_with_wrong_san_is_refused(self) -> None:
        bad = self.tmp / "certs-wrong-san"
        bad.mkdir()
        run(["bash", str(GEN_CA), "--cert-dir", str(bad)])
        run(["bash", str(GEN_CSR), "--cert-dir", str(bad), "--dns-name", WRONG_HOST])
        proc = run(["bash", str(GEN_CERT), "--cert-dir", str(bad), "--dns-name", DEFAULT_HOST])
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("REFUSING to sign", proc.stderr)
        self.assertFalse((bad / "server.crt").exists())


class TestGitIgnore(unittest.TestCase):
    def test_material_is_git_ignored_and_never_tracked(self) -> None:
        for name in ("ca.key", "server.key", "server.csr",
                     "ca.crt", "server.crt", "server.pem", "server_key.pem"):
            proc = run(["git", "-C", str(REPO_ROOT), "check-ignore", "--no-index",
                        f"server/certs/{name}"])
            self.assertEqual(proc.returncode, 0, f"git is not ignoring server/certs/{name}")

        tracked = run(["git", "-C", str(REPO_ROOT), "ls-files"])
        self.assertEqual(tracked.returncode, 0)
        secrets = [p for p in tracked.stdout.splitlines()
                   if p.endswith((".key", ".pem", ".p12"))]
        self.assertEqual(secrets, [], "private key material is tracked by git")


if __name__ == "__main__":
    unittest.main(verbosity=2)