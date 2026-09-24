#!/usr/bin/env python3
"""Delete a provisioned lamp so its one-shot enrollment can be tested again."""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from thingsboard_client import ThingsBoardError, ThingsBoardRestClient

DEFAULT_BASE_URL = "http://home-assistance.local:8080"
DEFAULT_API_KEY_FILE = "/home/dima/projects/hq_workspace/thingboard_tenant_api_key"


def read_api_key(path: Path) -> str:
    try:
        key = path.read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise ValueError(f"cannot read tenant API key file: {exc}") from None
    if not key:
        raise ValueError("tenant API key file is empty")
    return key


def reset(client: ThingsBoardRestClient, device_name: str) -> bool:
    device = client.find_device_by_name(device_name)
    if device is None:
        return False
    device_id = device.get("id", {}).get("id")
    if not device_id:
        raise ThingsBoardError("device lookup returned no device id")
    client.delete_device(device_id)
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=os.environ.get("TB_BASE_URL", DEFAULT_BASE_URL))
    parser.add_argument("--device-name", default=os.environ.get("TB_DEVICE_NAME"))
    parser.add_argument(
        "--api-key-file",
        default=os.environ.get("TB_TENANT_API_KEY_FILE", DEFAULT_API_KEY_FILE),
    )
    args = parser.parse_args(argv)

    if not args.device_name:
        print("TB_DEVICE_NAME or --device-name is required", file=sys.stderr)
        return 2
    try:
        client = ThingsBoardRestClient(args.base_url, read_api_key(Path(args.api_key_file)))
        deleted = reset(client, args.device_name)
    except (ThingsBoardError, ValueError) as exc:
        print(f"reset failed: {exc}", file=sys.stderr)
        return 1

    print("provisioned device deleted" if deleted else "provisioned device was already absent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())