#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT="${TB_DEVICE_PORT:-/dev/ttyUSB1}"
PAYLOAD_DIR="${TB_PAYLOAD_DIR:-/tmp/klc_payload}"
IMAGE_PATH="${TB_STORAGE_IMAGE:-/tmp/klc_storage.littlefs}"
LITTLEFS_TOOL="${LITTLEFS_TOOL:-${TB_LITTLEFS_TOOL:-littlefs_util}}"
SKIP_FLASH="${TB_SKIP_FLASH:-0}"

if ! command -v "$LITTLEFS_TOOL" >/dev/null 2>&1; then
  echo "littlefs_util was not found or is not executable: $LITTLEFS_TOOL" >&2
  echo "Install/build it, then set LITTLEFS_TOOL to its executable path." >&2
  echo "Source: $PROJECT_DIR/platform/hq_platform/tools/littlefs_util.c" >&2
  exit 1
fi

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

python3 "$PROJECT_DIR/scripts/thingsboard/generate_lamp_payload.py" "$PAYLOAD_DIR"

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