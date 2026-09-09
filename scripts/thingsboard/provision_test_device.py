#!/usr/bin/env python3
"""Idempotently create/find the ThingsBoard test device and store its token.

Usage:
    TB_API_KEY=... python3 provision_test_device.py [--device-name klc-test-01]

Never prints the tenant API key or the device access token; the token is
written only to a local, git-ignored file.
"""
from __future__ import annotations

import argparse
import os
import stat
import sys
from pathlib import Path

from thingsboard_client import ThingsBoardError, ThingsBoardRestClient

DEFAULT_BASE_URL = "http://home-assistance.local:8080"
DEFAULT_DEVICE_NAME = "klc-test-01"
DEFAULT_DEVICE_PROFILE = "default"


def token_path(device_name: str) -> Path:
    return Path(__file__).resolve().parent / f".{device_name}.token"


def find_or_create_device(client: ThingsBoardRestClient, name: str, profile: str) -> dict:
    device = client.find_device_by_name(name)
    if device is not None:
        return device
    return client.create_device(name, device_profile_name=profile)


def provision(client: ThingsBoardRestClient, device_name: str, device_profile: str) -> tuple[str, str]:
    """Returns (device_id, access_token) without ever logging the token."""
    device = find_or_create_device(client, device_name, device_profile)
    device_id = device["id"]["id"]
    credentials = client.get_device_credentials(device_id)
    if credentials.get("credentialsType") != "ACCESS_TOKEN":
        raise ThingsBoardError(
            f"device {device_name!r} does not use ACCESS_TOKEN credentials"
        )
    access_token = credentials["credentialsId"]
    path = token_path(device_name)
    path.write_text(access_token, encoding="utf-8")
    os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
    return device_id, str(path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=os.environ.get("TB_BASE_URL", DEFAULT_BASE_URL))
    parser.add_argument("--device-name", default=DEFAULT_DEVICE_NAME)
    parser.add_argument("--device-profile", default=DEFAULT_DEVICE_PROFILE)
    args = parser.parse_args(argv)

    api_key = os.environ.get("TB_API_KEY")
    if not api_key:
        print("TB_API_KEY environment variable is required", file=sys.stderr)
        return 2

    client = ThingsBoardRestClient(args.base_url, api_key)
    try:
        device_id, path = provision(client, args.device_name, args.device_profile)
    except ThingsBoardError as exc:
        print(f"provisioning failed: {exc}", file=sys.stderr)
        return 1

    print(f"device {args.device_name!r} ready: id={device_id}")
    print(f"access token stored at {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
