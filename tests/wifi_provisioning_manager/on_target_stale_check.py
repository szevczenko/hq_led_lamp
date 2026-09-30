#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Automated on-target stale-credential fallback check (TASK-139).

The core bug report scenario: a device with a saved credential that cannot
connect (router replaced / password changed) must NOT park forever with no
AP — the platform fallback controller's bounded budget
(CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS, 2 in this product) must
open the provisioning portal after that many CONNECT_FAILED episodes.

This script is the automated verification step for that scenario, run on
the real hardware through the standard serial flash/monitor loop:

  1. build a LittleFS storage image seeding a DELIBERATELY WRONG saved
     credential (``wifi_ap.json``, a documented throwaway SSID+password)
     next to the four config documents and the CA that the CONFIGURATION
     gate requires (the fresh payload from the manual procedure, TASK-128),
  2. write that image at the storage partition offset (0x3A0000) over
     serial — the same path B the erase/re-provision escape hatch
     (TASK-137) uses, only seeding a bogus credential instead of erasing,
  3. flash the application (app flash never touches the storage partition,
     so the wrong credential survives the flash),
  4. capture the bounded monitor log,
  5. assert the bounded fallback through on_target_smoke_check.py
     (``--expect stale-credential``): one or more NETWORK_FAILED episodes
     up to the configured budget, then PROVISIONING_STARTED -> PROVISIONING
     with the TASK-135 portal-reachability signature, exactly one ENTER and
     exactly one portal-up period (no restart storm), backtrace-free, and
     neither the stale nor the (future) submitted credential anywhere.

The MANUAL STEP — a human joins the provisioning AP with a phone/laptop and
submits the correct credential; the dev workstation's Wi-Fi stays untouched
— cannot be automated by this script (it needs a second radio and a lab
WLAN).  It is documented in the component README; capture its monitor log
and assert the full recovery with::

    python3 on_target_smoke_check.py --log <recovery.log> \\
        --expect stale-recovery \\
        --stale-ssid <stale-ssid> --stale-password <stale-password> \\
        --test-ssid <lab-ssid> --test-password <lab-password>

Usage
-----
Run everything (storage write + app flash + bounded monitor + assertions)::

    source .../esp-idf/export.sh   # idf.py must be on PATH
    python3 tests/wifi_provisioning_manager/on_target_stale_check.py \\
        --port /dev/ttyUSB1 --monitor-duration 90 \\
        --stale-ssid KLC-Stale-Net-139 --stale-password KLC-Stale-Pass-139!

Exit status is 0 when the storage image round-trips, the boot reaches the
portal after the bounded budget and every assertion passes, non-zero
otherwise.
"""

import argparse
import json
import os
import pty
import select
import shutil
import subprocess
import sys
import tempfile

# Import the checker's shared constants/machinery from the same directory.
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import on_target_smoke_check as smoke  # noqa: E402

# Storage geometry (partitions.csv): the LittleFS "storage" partition.
STORAGE_OFFSET = "0x3A0000"
STORAGE_SIZE = 0x60000            # 393216 B = 96 * 4096-B blocks

# Canonical config payload the fresh manual procedure builds (TASK-128):
# four config documents + the CA at /cert/ca.crt — no wifi_ap.json.
DEFAULT_PAYLOAD = "/tmp/klc_payload"
DEFAULT_OUT_IMAGE = "/tmp/klc_storage_stale.littlefs"


def build_stale_image(payload, stale_ssid, stale_password, out_image):
    """Seed the wrong credential into a copy of ``payload`` and build the
    LittleFS image at ``out_image``.  Returns the image path.

    The stale credential is a documented throwaway value (never the real
    home secrets).  ``wifi_ap.json`` uses the platform persistence schema
    (wifi_config.c): ``{"last_use":0,"credentials":[{"nb":0,"ssid":…,
    "password":…}]}``.
    """
    if not os.path.isdir(payload):
        sys.stderr.write(
            "error: payload directory %r not found — build it per the "
            "manual procedure (TASK-128 step 1): four config documents + "
            "cert/ca.crt, no wifi_ap.json\n" % payload
        )
        sys.exit(2)

    with tempfile.TemporaryDirectory(prefix="klc_stale_payload_") as tmp:
        for name in os.listdir(payload):
            src = os.path.join(payload, name)
            dst = os.path.join(tmp, name)
            if os.path.isdir(src):
                shutil.copytree(src, dst)
            else:
                shutil.copy2(src, dst)

        wifi_ap = {
            "last_use": 0,
            "credentials": [
                {"nb": 0, "ssid": stale_ssid, "password": stale_password},
            ],
        }
        with open(os.path.join(tmp, "wifi_ap.json"), "w",
                  encoding="utf-8") as fh:
            fh.write(json.dumps(wifi_ap, separators=(",", ":")))

        littlefs_util = shutil.which("littlefs_util") or "/tmp/littlefs_util"
        if not os.path.exists(littlefs_util):
            sys.stderr.write(
                "error: %r missing — build it per the manual procedure "
                "(platform/hq_platform/tools/littlefs_util.c, gcc -std=gnu99 "
                "...)\n" % littlefs_util
            )
            sys.exit(2)

        subprocess.check_call(
            [littlefs_util, "--create", tmp, "--out", out_image,
             "--size", str(STORAGE_SIZE)],
            stdout=sys.stdout, stderr=sys.stderr,
        )
    return out_image


def write_storage(out_image, port, offset=STORAGE_OFFSET):
    """Write the stale storage image at the storage partition offset."""
    subprocess.check_call(
        ["python", "-m", "esptool", "--chip", "esp32", "-p", port,
         "-b", "460800", "--before", "default_reset", "--after",
         "hard_reset", "write_flash", "--flash_mode", "dio",
         "--flash_size", "4MB", "--flash_freq", "40m",
         offset, out_image],
        stdout=sys.stdout, stderr=sys.stderr,
    )


def _capture_monitor_pty(port, duration):
    """Run ``timeout <duration> idf.py monitor`` on a pty and return the
    captured text.

    ``idf.py monitor`` refuses to run when its standard input is not a TTY
    ("Monitor requires standard input to be attached to TTY"), so a bare
    subprocess capture fails; running the monitor on a pseudo-terminal
    satisfies the check AND captures the device output the same way the
    interactive ``timeout 1m idf.py monitor | tee`` loop does.
    """
    master, slave = pty.openpty()
    try:
        proc = subprocess.Popen(
            ["timeout", str(duration), "idf.py", "-p", port, "monitor"],
            stdin=slave, stdout=slave, stderr=slave, close_fds=True,
        )
    finally:
        os.close(slave)

    chunks = []
    while proc.poll() is None:
        ready, _, _ = select.select([master], [], [], 1.0)
        if ready:
            try:
                data = os.read(master, 4096)
            except OSError:
                break
            if not data:
                break
            chunks.append(data)

    # Drain anything the pty still holds after the child exited.
    try:
        while True:
            data = os.read(master, 4096)
            if not data:
                break
            chunks.append(data)
    except OSError:
        pass
    os.close(master)
    proc.wait()
    text = b"".join(chunks).decode("utf-8", errors="replace")
    # The pty translates \n to \r\n; normalize so line matching is exact.
    return text.replace("\r\n", "\n").replace("\r", "\n")


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="On-target stale-credential fallback check (TASK-139).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--payload",
        default=DEFAULT_PAYLOAD,
        help="Directory with the four config documents + cert/ca.crt "
             "(no wifi_ap.json) from the manual procedure (TASK-128).",
    )
    parser.add_argument(
        "--stale-ssid",
        default=smoke.DEFAULT_STALE_SSID,
        help="Deliberately WRONG SSID to seed (documented throwaway).",
    )
    parser.add_argument(
        "--stale-password",
        default=smoke.DEFAULT_STALE_PASSWORD,
        help="Deliberately WRONG password to seed (documented throwaway).",
    )
    parser.add_argument(
        "--fallback-budget",
        type=int,
        default=smoke.DEFAULT_FALLBACK_BUDGET,
        help="Configured CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS "
             "budget the smoke check asserts against (consecutive "
             "NETWORK_FAILED episodes before the portal must open).",
    )
    parser.add_argument(
        "--out-image",
        default=DEFAULT_OUT_IMAGE,
        help="Output LittleFS storage image path.",
    )
    parser.add_argument(
        "--storage-offset",
        default=STORAGE_OFFSET,
        help="Storage partition flash offset (partitions.csv).",
    )
    parser.add_argument(
        "--port",
        default="/dev/ttyUSB1",
        help="Serial port for storage write / flash / monitor.",
    )
    parser.add_argument(
        "--monitor-duration",
        type=int,
        default=90,
        help="Bounded monitor window in seconds (must cover the fallback "
             "budget: a session lasts up to NETWORK_CONNECT_TIMEOUT_MS "
             "30 s plus the retry backoff, so >=90 s is recommended).",
    )
    parser.add_argument(
        "--flash-storage",
        action="store_true",
        default=True,
        help="Write the seeded storage image before flashing the app "
             "(default).",
    )
    parser.add_argument(
        "--no-flash-storage",
        dest="flash_storage",
        action="store_false",
        help="Skip the storage write (image already on the device).",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    if args.flash_storage:
        print("== [1/4] building the stale-credential storage image ==")
        build_stale_image(args.payload, args.stale_ssid,
                          args.stale_password, args.out_image)
        print("== [2/4] writing the storage image at %s =="
              % args.storage_offset)
        write_storage(args.out_image, args.port, args.storage_offset)

    print("== [3/4] app flash + bounded monitor capture ==")
    subprocess.check_call(
        ["idf.py", "flash", "-p", args.port],
        stdout=sys.stdout, stderr=sys.stderr,
    )
    text = _capture_monitor_pty(args.port, args.monitor_duration)

    log_path = "/tmp/hq_led_lamp_esp.log"
    with open(log_path, "w", encoding="utf-8") as fh:
        fh.write(text)

    print("== [4/4] stale-credential assertions on %s ==" % log_path)
    rc = smoke.main([
        "--log", log_path,
        "--expect", "stale-credential",
        "--stale-ssid", args.stale_ssid,
        "--stale-password", args.stale_password,
        "--fallback-budget", str(args.fallback_budget),
    ])
    if rc != 0:
        sys.stderr.write(
            "stale-credential assertions failed; see the smoke-check "
            "FAIL lines above.  A human must then run the MANUAL STEP "
            "(join the provisioning AP with a phone/laptop, submit the "
            "correct credential) and re-run on_target_smoke_check.py "
            "--expect stale-recovery on that session's log.\n"
        )
        return rc

    print(
        "PASS: stale-credential device went through the bounded "
        "NETWORK_FAILED budget and opened the provisioning portal "
        "(exactly one ENTER, one portal-up period, no Backtrace, no "
        "credential content).  Manual recovery step remains: documented in "
        "the component README.\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())