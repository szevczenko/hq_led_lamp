#!/usr/bin/env python3
"""Deploy a firmware image to the lamp through ThingsBoard OTA.

Subcommands (tenant API key from $TB_API_KEY or --api-key-file):

  deploy    create/upload an OTA package for the image, assign it to the
            device's profile and follow the device's fw_state until
            UPDATED (with the expected version) or FAILED.
  status    print the device's firmware telemetry.
  unassign  clear the device profile's firmware assignment.
  delete    delete an (unassigned) OTA package by title+version.

Title and version default to the ESP-IDF app descriptor embedded in the
image, i.e. exactly what the device reports as fw_title / fw_version after
booting it.

  TB_API_KEY=$(cat caredentials/thingboard_token) \\
      python3 scripts/thingsboard/ota_deploy.py deploy \\
      --device klc-kitchen-99 --file /tmp/klc_1.0.1.bin

Assigning firmware to a device profile targets EVERY device of that
profile; deploy refuses when other devices share the profile unless
--allow-shared-profile is given.  Never prints the API key.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from thingsboard_client import ThingsBoardError, ThingsBoardRestClient  # noqa: E402

DEFAULT_BASE_URL = "http://home-assistance.local:8080"
DEFAULT_API_KEY_FILE = Path(__file__).resolve().parents[2] / "caredentials" / "thingboard_token"
FW_KEYS = ["fw_state", "fw_error", "fw_title", "fw_version"]

# esp_image_header_t (24 B) + first esp_image_segment_header_t (8 B).
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432


def read_app_descriptor(image: bytes) -> tuple[str, str]:
    """Returns (project_name, version) from an ESP-IDF application image."""
    if len(image) < APP_DESC_OFFSET + 80 or image[0] != 0xE9:
        raise SystemExit("error: not an ESP-IDF application image")
    (magic,) = struct.unpack_from("<I", image, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        raise SystemExit("error: esp_app_desc_t magic not found")
    version = image[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48].split(b"\0", 1)[0]
    name = image[APP_DESC_OFFSET + 48:APP_DESC_OFFSET + 80].split(b"\0", 1)[0]
    return name.decode("ascii"), version.decode("ascii")


def corrupt_image(image: bytes) -> bytes:
    """Flip one payload byte (header intact) so on-device validation fails."""
    data = bytearray(image)
    index = len(data) // 2
    data[index] ^= 0xFF
    return bytes(data)


def load_api_key(args: argparse.Namespace) -> str:
    key = os.environ.get("TB_API_KEY")
    if not key:
        path = Path(args.api_key_file)
        if not path.is_file():
            raise SystemExit("error: set TB_API_KEY or provide --api-key-file")
        key = path.read_text(encoding="utf-8").strip()
    if not key:
        raise SystemExit("error: empty ThingsBoard API key")
    return key


def get_device(client: ThingsBoardRestClient, name: str) -> dict:
    device = client.find_device_by_name(name)
    if device is None:
        raise SystemExit(f"error: device {name!r} not found")
    return device


def find_package(client: ThingsBoardRestClient, title: str, version: str) -> dict | None:
    for package in client.list_ota_packages():
        if package.get("title") == title and package.get("version") == version:
            return package
    return None


def print_status(client: ThingsBoardRestClient, device_id: str) -> dict:
    values = client.get_latest_timeseries_with_ts(device_id, FW_KEYS)
    for key in FW_KEYS:
        if key in values:
            ts, value = values[key]
            stamp = time.strftime("%H:%M:%S", time.localtime(ts / 1000))
            print(f"  {key:20s} = {value!r} (at {stamp})")
    return values


def set_profile_firmware(client: ThingsBoardRestClient, profile_id: str,
                         package_id: str | None) -> None:
    profile = client.get_device_profile(profile_id)
    profile["firmwareId"] = (
        {"entityType": "OTA_PACKAGE", "id": package_id} if package_id else None
    )
    client.save_device_profile(profile)


def follow(client: ThingsBoardRestClient, device_id: str, version: str,
           since_ms: int, timeout_s: int) -> int:
    """Follow device fw_state until UPDATED@version / FAILED / timeout."""
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        values = client.get_latest_timeseries_with_ts(device_id, FW_KEYS)
        state = values.get("fw_state")
        current = values.get("fw_version", (0, None))[1]
        if state is not None and state[0] >= since_ms:
            snapshot = (state[1], current, values.get("fw_error", (0, ""))[1])
            if snapshot != last:
                last = snapshot
                print(f"[deploy] fw_state={snapshot[0]} fw_version={snapshot[1]}"
                      + (f" fw_error={snapshot[2]!r}" if snapshot[2] else ""), flush=True)
            if state[1] == "UPDATED" and current == version:
                print(f"PASS: device reports UPDATED, fw_version={version}")
                return 0
            if state[1] == "FAILED":
                print(f"FAILED: device reported fw_error={snapshot[2]!r}")
                return 3
        time.sleep(3)
    print(f"TIMEOUT: no UPDATED/FAILED within {timeout_s} s (last={last})")
    return 4


def cmd_deploy(client: ThingsBoardRestClient, args: argparse.Namespace) -> int:
    image = Path(args.file).read_bytes()
    title, version = read_app_descriptor(image)
    title = args.title or title
    version = args.version or version
    if args.corrupt:
        image = corrupt_image(image)
    device = get_device(client, args.device)
    profile_id = device["deviceProfileId"]["id"]
    others = [d["name"] for d in client.list_devices_by_profile_name(device["type"])
              if d["id"]["id"] != device["id"]["id"]]
    if others and not args.allow_shared_profile:
        raise SystemExit(
            f"error: profile {device['type']!r} is shared with {others}; they "
            "would be updated too (use --allow-shared-profile)")

    print(f"[deploy] image {args.file}: title={title} version={version} "
          f"size={len(image)}{' (CORRUPTED copy)' if args.corrupt else ''}")
    package = find_package(client, title, version)
    if package is None:
        package = client.create_ota_package(title, version, profile_id)
        print(f"[deploy] created OTA package {package['id']['id']}")
    if not package.get("hasData"):
        package = client.upload_ota_package_data(
            package["id"]["id"], f"{title}-{version}.bin", image)
        print(f"[deploy] uploaded {package.get('dataSize')} bytes, "
              f"checksum {package.get('checksumAlgorithm')}={package.get('checksum')}")
    else:
        print("[deploy] package already has data; reusing it")

    since_ms = int(time.time() * 1000)
    set_profile_firmware(client, profile_id, package["id"]["id"])
    print(f"[deploy] assigned to device profile {device['type']!r}; following "
          f"{args.device} for up to {args.wait} s")
    if args.wait <= 0:
        return 0
    return follow(client, device["id"]["id"], version, since_ms, args.wait)


def cmd_status(client: ThingsBoardRestClient, args: argparse.Namespace) -> int:
    device = get_device(client, args.device)
    profile = client.get_device_profile(device["deviceProfileId"]["id"])
    print(f"device {args.device} profile={device['type']} "
          f"profile.firmwareId={(profile.get('firmwareId') or {}).get('id')} "
          f"device.firmwareId={(device.get('firmwareId') or {}).get('id')}")
    print_status(client, device["id"]["id"])
    return 0


def cmd_unassign(client: ThingsBoardRestClient, args: argparse.Namespace) -> int:
    device = get_device(client, args.device)
    set_profile_firmware(client, device["deviceProfileId"]["id"], None)
    print(f"[deploy] cleared firmware assignment of profile {device['type']!r}")
    return 0


def cmd_delete(client: ThingsBoardRestClient, args: argparse.Namespace) -> int:
    package = find_package(client, args.title, args.version)
    if package is None:
        print(f"no package {args.title} {args.version}")
        return 0
    client.delete_ota_package(package["id"]["id"])
    print(f"[deploy] deleted package {args.title} {args.version}")
    return 0


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base-url", default=os.environ.get("TB_BASE_URL", DEFAULT_BASE_URL))
    parser.add_argument("--api-key-file", default=str(DEFAULT_API_KEY_FILE))
    sub = parser.add_subparsers(dest="command", required=True)

    deploy = sub.add_parser("deploy", help="upload + assign + follow")
    deploy.add_argument("--device", required=True)
    deploy.add_argument("--file", required=True, help="ESP-IDF application .bin")
    deploy.add_argument("--title", help="override the image's project name")
    deploy.add_argument("--version", help="override the image's version")
    deploy.add_argument("--wait", type=int, default=600, help="follow timeout (s); 0 = no wait")
    deploy.add_argument("--corrupt", action="store_true",
                        help="upload a corrupted copy (negative test; use a distinct --version)")
    deploy.add_argument("--allow-shared-profile", action="store_true")

    status = sub.add_parser("status", help="print firmware telemetry")
    status.add_argument("--device", required=True)

    unassign = sub.add_parser("unassign", help="clear the profile firmware assignment")
    unassign.add_argument("--device", required=True)

    delete = sub.add_parser("delete", help="delete an OTA package")
    delete.add_argument("--title", required=True)
    delete.add_argument("--version", required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    client = ThingsBoardRestClient(args.base_url, load_api_key(args))
    handlers = {"deploy": cmd_deploy, "status": cmd_status,
                "unassign": cmd_unassign, "delete": cmd_delete}
    try:
        return handlers[args.command](client, args)
    except ThingsBoardError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
