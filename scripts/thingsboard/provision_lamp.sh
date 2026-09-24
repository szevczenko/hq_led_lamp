#!/usr/bin/env bash
set -Eeuo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PLATFORM_DIR="$PROJECT_DIR/platform/hq_platform"
PORT="${TB_DEVICE_PORT:-/dev/ttyUSB1}"
PAYLOAD_DIR="${TB_PAYLOAD_DIR:-/tmp/klc_payload}"
IMAGE_PATH="${TB_STORAGE_IMAGE:-/tmp/klc_storage.littlefs}"
LITTLEFS_TOOL="${TB_LITTLEFS_TOOL:-/tmp/littlefs_util}"
SKIP_FLASH="${TB_SKIP_FLASH:-0}"

if fuser -s "$PORT"; then
  echo "Serial port $PORT is busy. Stop idf.py monitor before provisioning." >&2
  exit 1
fi

if [[ ! -f "$PROJECT_DIR/server/certs/ca.crt" ]]; then
  echo "Missing CA certificate: $PROJECT_DIR/server/certs/ca.crt" >&2
  exit 1
fi

required_vars=(WIFI_SSID WIFI_PASSWORD TB_DEVICE_NAME TB_PROVISION_DEVICE_KEY TB_PROVISION_DEVICE_SECRET)
for variable in "${required_vars[@]}"; do
  if [[ -z "${!variable:-}" ]]; then
    echo "$variable must be exported before provisioning." >&2
    echo "Example: source device/device_credentials.sh" >&2
    exit 1
  fi
done

if [[ "$TB_DEVICE_NAME" =~ [[:cntrl:]] || "$TB_PROVISION_DEVICE_KEY" =~ [[:space:]] ||
      "$TB_PROVISION_DEVICE_SECRET" =~ [[:space:]] || "$WIFI_SSID" =~ [[:cntrl:]] ||
      "$WIFI_PASSWORD" =~ [[:cntrl:]] ]]; then
  echo "Provisioning values must not contain invalid whitespace/control characters." >&2
  echo "Example: source device/device_credentials.sh" >&2
  exit 1
fi
export WIFI_SSID WIFI_PASSWORD TB_DEVICE_NAME TB_PROVISION_DEVICE_KEY TB_PROVISION_DEVICE_SECRET

rm -rf "$PAYLOAD_DIR" "$IMAGE_PATH"
mkdir -p "$PAYLOAD_DIR/config" "$PAYLOAD_DIR/cert"
cp "$PROJECT_DIR/server/certs/ca.crt" "$PAYLOAD_DIR/cert/ca.crt"

python3 - "$PAYLOAD_DIR" <<'PY'
import json
import os
import sys
from pathlib import Path

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
    path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")
PY

cd "$PLATFORM_DIR/tools"
gcc -std=gnu99 -o "$LITTLEFS_TOOL" littlefs_util.c \
  ../third_party/littlefs/lfs.c \
  ../third_party/littlefs/lfs_util.c \
  ../third_party/littlefs/bd/lfs_filebd.c \
  -I ../third_party/littlefs \
  -I ../third_party/littlefs/bd

"$LITTLEFS_TOOL" --create "$PAYLOAD_DIR" \
  --out "$IMAGE_PATH" \
  --size 393216

"$LITTLEFS_TOOL" --tree / --in "$IMAGE_PATH"

cd "$PROJECT_DIR"
if [[ "$SKIP_FLASH" == "1" ]]; then
  echo "LittleFS image generated at $IMAGE_PATH (flash skipped)."
else
  python3 -m esptool --chip esp32 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 4MB --flash_freq 40m \
    0x3A0000 "$IMAGE_PATH"
fi

echo "Lamp bootstrap storage provisioned at 0x3A0000. Reset the device and monitor enrollment."