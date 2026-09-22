#!/usr/bin/env bash
set -Eeuo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PLATFORM_DIR="$PROJECT_DIR/platform/hq_platform"
PORT="${TB_DEVICE_PORT:-/dev/ttyUSB1}"
PAYLOAD_DIR="${TB_PAYLOAD_DIR:-/tmp/klc_payload}"
IMAGE_PATH="${TB_STORAGE_IMAGE:-/tmp/klc_storage.littlefs}"
LITTLEFS_TOOL="${TB_LITTLEFS_TOOL:-/tmp/littlefs_util}"

if fuser -s "$PORT"; then
  echo "Serial port $PORT is busy. Stop idf.py monitor before provisioning." >&2
  exit 1
fi

if [[ ! -f "$PROJECT_DIR/server/certs/ca.crt" ]]; then
  echo "Missing CA certificate: $PROJECT_DIR/server/certs/ca.crt" >&2
  exit 1
fi

read -rsp "ThingsBoard device access token: " TB_DEVICE_TOKEN
printf '\n'
if [[ -z "$TB_DEVICE_TOKEN" ]]; then
  echo "The device access token cannot be empty." >&2
  exit 1
fi
if [[ "$TB_DEVICE_TOKEN" =~ [[:space:]] ]]; then
  echo "The device access token must not contain whitespace." >&2
  exit 1
fi
export TB_DEVICE_TOKEN

rm -rf "$PAYLOAD_DIR" "$IMAGE_PATH"
mkdir -p "$PAYLOAD_DIR/config" "$PAYLOAD_DIR/cert"
cp "$PROJECT_DIR/server/certs/ca.crt" "$PAYLOAD_DIR/cert/ca.crt"

python3 - "$PAYLOAD_DIR" <<'PY'
import json
import os
import sys
from pathlib import Path

root = Path(sys.argv[1])
token = os.environ["TB_DEVICE_TOKEN"]

documents = {
    "config/device.json": {
        "schema_version": 1,
        "product": "HQ Lamp",
        "hardware_revision": "B",
        "serial": "klc-kitchen-01",
        "thingsboard_name": "klc-kitchen-01",
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
        "client_id": "klc-kitchen-01",
        "auth_mode": "access_token",
        "skip_verify": False,
    },
    "config/identity.json": {
        "schema_version": 1,
        "client_id": "klc-kitchen-01",
        "access_token": token,
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
python3 -m esptool --chip esp32 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x3A0000 "$IMAGE_PATH"

unset TB_DEVICE_TOKEN
echo "Lamp storage provisioned at 0x3A0000. Reset the device and monitor its MQTT connection."