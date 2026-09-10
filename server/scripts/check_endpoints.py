#!/usr/bin/env python3
"""Credential-free validation of the development ThingsBoard endpoints.

Verifies, without any API key, device token, or login:

  1. HTTPS (port 443): TLS handshake validates the server chain against
     server/certs/ca.crt and the hostname thingsboard.home.arpa, then an
     HTTP GET / must answer 2xx/3xx (ThingsBoard login page through Caddy).
  2. MQTT TLS (port 8883): TLS handshake validated the same way, then an
     anonymous MQTT 3.1.1 CONNECT must be answered with a CONNACK. Any
     CONNACK return code counts as success - a "not authorized" code still
     proves the broker's TLS MQTT endpoint is reachable and speaking MQTT.

Usage:
  python3 server/scripts/check_endpoints.py [options]

Options:
  --host NAME      hostname to verify in the certificates
                   (default: TB_DNS_NAME from server/.env, else thingsboard.home.arpa)
  --ip ADDR        address to connect to while still verifying --host
                   (default: resolve --host via DNS; use for /etc/hosts or
                   loopback smoke tests, e.g. --ip 127.0.0.1)
  --https-port P   HTTPS port to check (default: TB_HTTPS_PORT or 443)
  --mqtt-port P    MQTT TLS port to check (default: TB_MQTT_TLS_PORT or 8883)
  --ca PATH        development root CA (default: server/certs/ca.crt)
  --wait SECONDS   poll the checks until success or SECONDS elapsed
                   (handy right after `docker compose up -d`)
  --connect-timeout SECONDS   per-connection timeout (default: 10)

Exit code 0 when every check passes; 1 otherwise. Output is a per-check
PASS/FAIL report. No credentials are ever required or printed.
"""
from __future__ import annotations

import argparse
import socket
import ssl
import sys
import time
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parents[1]
DEFAULT_CA = SERVER_DIR / "certs" / "ca.crt"
ENV_FILE = SERVER_DIR / ".env"

CONNACK_OK_RETURN_CODES = {0, 1, 2, 3, 4, 5}
MQTT_RETURN_CODE_NAMES = {
    0: "connection accepted (anonymous login allowed)",
    1: "unacceptable protocol version",
    2: "identifier rejected",
    3: "server unavailable",
    4: "bad username or password",
    5: "not authorized (expected for anonymous access)",
}


def load_env_defaults() -> dict:
    """Read non-secret defaults used by the compose file from server/.env."""
    defaults: dict = {}
    if not ENV_FILE.is_file():
        return defaults
    for line in ENV_FILE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        if key in ("TB_DNS_NAME", "TB_HTTPS_PORT", "TB_MQTT_TLS_PORT"):
            defaults[key] = value
    return defaults


def parse_args() -> argparse.Namespace:
    env = load_env_defaults()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=env.get("TB_DNS_NAME", "thingsboard.home.arpa"))
    parser.add_argument("--ip", default=None, help="connect address override (see docstring)")
    parser.add_argument("--https-port", type=int, default=int(env.get("TB_HTTPS_PORT", "443")))
    parser.add_argument("--mqtt-port", type=int, default=int(env.get("TB_MQTT_TLS_PORT", "8883")))
    parser.add_argument("--ca", default=str(DEFAULT_CA))
    parser.add_argument("--wait", type=int, default=0, help="poll up to N seconds")
    parser.add_argument("--connect-timeout", type=float, default=10.0)
    return parser.parse_args()


def tls_context(ca_path: str) -> ssl.SSLContext:
    ctx = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=ca_path)
    ctx.check_hostname = True  # verified chain AND DNS hostname match
    ctx.verify_mode = ssl.CERT_REQUIRED
    return ctx


def connect_tls(host: str, port: int, ip: str | None, ca_path: str, timeout: float) -> ssl.SSLSocket:
    target = ip if ip else host
    ctx = tls_context(ca_path)
    raw = socket.create_connection((target, port), timeout=timeout)
    try:
        return ctx.wrap_socket(raw, server_hostname=host)
    except Exception:
        raw.close()
        raise


def check_https(host: str, port: int, ip: str | None, ca_path: str, timeout: float) -> str:
    with connect_tls(host, port, ip, ca_path, timeout) as tls:
        request = (
            f"GET /login HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n"
        ).encode("ascii")
        tls.sendall(request)
        data = b""
        while True:
            chunk = tls.recv(4096)
            if not chunk:
                break
            data += chunk
            if b"\r\n\r\n" in data or len(data) > 65536:
                break
        head, _, _ = data.partition(b"\r\n")
        status_line = head.decode("iso-8859-1", errors="replace")
        code = int(status_line.split(" ", 2)[1]) if len(status_line.split(" ", 2)) >= 2 else 0
        if not (200 <= code < 400):
            raise AssertionError(f"unexpected HTTP status {status_line!r}")
        return status_line


def mqtt_connect_packet(client_id: str = "hq-endpoint-check") -> bytes:
    protocol = b"\x00\x04MQTT"
    connect_flags = b"\x02"  # clean session, no username/password
    keepalive = (60).to_bytes(2, "big")
    cid = len(client_id).to_bytes(2, "big") + client_id.encode("ascii")
    payload = protocol + b"\x04" + connect_flags + keepalive + cid
    assert len(payload) < 128, "test packet must fit single-byte remaining length"
    return b"\x10" + bytes([len(payload)]) + payload


def check_mqtt_tls(host: str, port: int, ip: str | None, ca_path: str, timeout: float) -> str:
    with connect_tls(host, port, ip, ca_path, timeout) as tls:
        tls.sendall(mqtt_connect_packet())
        response = tls.recv(16)
        if len(response) < 4 or response[0] != 0x20:
            raise AssertionError(
                f"expected an MQTT CONNACK (0x20), got {response.hex()!r}"
            )
        return_code = response[3]
        if return_code not in CONNACK_OK_RETURN_CODES:
            raise AssertionError(f"unexpected MQTT CONNACK return code {return_code}")
        return f"CONNACK rc={return_code} ({MQTT_RETURN_CODE_NAMES[return_code]})"


def run_all(args: argparse.Namespace) -> list[bool]:
    ca_path = Path(args.ca)
    if not ca_path.is_file():
        print(f"FAIL  CA file not found: {ca_path} (run server/scripts/gen_dev_tls.sh)")
        return [False]

    results = []
    print(f"Verify CA: {ca_path}")
    try:
        detail = check_https(args.host, args.https_port, args.ip, str(ca_path), args.connect_timeout)
        print(f"PASS  HTTPS {args.host}:{args.https_port} -> {detail}")
        results.append(True)
    except Exception as exc:  # noqa: BLE001 - report any endpoint failure
        print(f"FAIL  HTTPS {args.host}:{args.https_port} -> {exc}")
        results.append(False)

    try:
        detail = check_mqtt_tls(args.host, args.mqtt_port, args.ip, str(ca_path), args.connect_timeout)
        print(f"PASS  MQTT TLS {args.host}:{args.mqtt_port} -> {detail}")
        results.append(True)
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL  MQTT TLS {args.host}:{args.mqtt_port} -> {exc}")
        results.append(False)

    return results


def main(argv: list[str] | None = None) -> int:
    args = parse_args()
    deadline = time.monotonic() + args.wait if args.wait > 0 else None

    while True:
        results = run_all(args)
        if all(results):
            return 0
        if deadline is None or time.monotonic() >= deadline:
            print("endpoint validation FAILED")
            return 1
        remaining = int(deadline - time.monotonic())
        print(f"retrying in 10s ({remaining}s left)...")
        time.sleep(10)


if __name__ == "__main__":
    raise SystemExit(main())