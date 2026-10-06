#!/usr/bin/env python3
"""Generate the configuration documents included in a lamp's LittleFS image."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PAYLOAD_DIR", file=sys.stderr)
        return 2

    root = Path(sys.argv[1])
    device_name = os.environ["TB_DEVICE_NAME"]
    provision_key = os.environ["TB_PROVISION_DEVICE_KEY"]
    provision_secret = os.environ["TB_PROVISION_DEVICE_SECRET"]
    wifi_ssid = os.environ["WIFI_SSID"]
    wifi_password = os.environ["WIFI_PASSWORD"]

    documents = {
        "config/device.json": {
            "schema_version": 1,
            "product": "HQ Lamp",
            "hardware_revision": "B",
            "serial": device_name,
            "thingsboard_name": device_name,
        },
        "config/manufacturing.json": {
            "schema_version": 1,
            "manufacturing_state": 1,
            "credential_mode": 1,
        },
        "config/mqtt.json": {
            "schema_version": 1,
            "hostname": "home-assistance.local",
            "port": 8883,
            "tls_mode": "mqtts",
            "ca_path": "/cert/ca.crt",
            "client_id": device_name,
            "auth_mode": "runtime",
            "skip_verify": False,
        },
        "config/provisioning.json": {
            "schema_version": 1,
            "device_name": device_name,
            "provision_device_key": provision_key,
            "provision_device_secret": provision_secret,
        },
        "wifi_ap.json": {
            "last_use": 0,
            "credentials": [
                {"nb": 0, "ssid": wifi_ssid, "password": wifi_password},
            ],
        },
    }

    for relative_path, document in documents.items():
        path = root / relative_path
        path.write_text(
            json.dumps(document, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())