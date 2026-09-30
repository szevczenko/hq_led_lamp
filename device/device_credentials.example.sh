#!/usr/bin/env bash

# Copy this file to device/device_credentials.sh, edit the values, then run:
#   source device/device_credentials.sh
#   ./scripts/thingsboard/provision_lamp.sh
#
# Keep device/device_credentials.sh private. It is ignored by git.

export WIFI_SSID="replace-with-wifi-ssid"
export WIFI_PASSWORD="replace-with-wifi-password"
export TB_DEVICE_NAME="klc-kitchen-01"
export TB_PROVISION_DEVICE_KEY="replace-with-provision-device-key"
export TB_PROVISION_DEVICE_SECRET="replace-with-provision-device-secret"
