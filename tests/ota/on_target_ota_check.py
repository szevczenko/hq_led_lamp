#!/usr/bin/env python3
"""On-target ThingsBoard OTA check for the Kitchen LED Controller.

Captures the device's serial log through ``idf.py monitor`` (on a pty, the
same loop as ``timeout 1m idf.py monitor | tee``), optionally triggers a
ThingsBoard OTA deployment once the device's firmware updater is ready, and
asserts the stable ``[ota]`` / ``Firmware:`` log markers.

Expectations (``--expect``):

  boot     the running image reports --from-version, the updater comes up
           and a firmware check is requested.
  update   --from-version downloads --to-version (blinking OTA state),
           activates it, restarts, boots --to-version, reports UPDATED and
           re-applies the lamp state.
  failed   the download of --to-version fails, the device returns online
           and restores the lamp state without restarting.

Usage (idf.py on PATH; TB_API_KEY or caredentials/thingboard_token)::

    python3 tests/ota/on_target_ota_check.py --port /dev/ttyUSB1 \\
        --expect update --from-version 1.0.0 --to-version 1.0.1 \\
        --deploy-file /tmp/klc_1.0.1.bin --device klc-kitchen-99 \\
        --duration 420

    python3 tests/ota/on_target_ota_check.py --log /tmp/klc_ota.log \\
        --expect update --from-version 1.0.0 --to-version 1.0.1

Exit status 0 when every assertion passes.
"""
from __future__ import annotations

import argparse
import os
import pty
import re
import select
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEPLOY = REPO / "scripts" / "thingsboard" / "ota_deploy.py"
TITLE = "kitchen_led_controller"
READY = "[ota] firmware updater ready"
CRASH_PATTERNS = ("Backtrace:", "Guru Meditation", "abort() was called",
                  "Task watchdog got triggered", "stack overflow")


class MonitorCapture:
    """``timeout <duration> idf.py -p <port> monitor`` on a pty, streamed."""

    def __init__(self, port: str, duration: int) -> None:
        master, slave = pty.openpty()
        self._proc = subprocess.Popen(
            ["timeout", str(duration), "idf.py", "-p", port, "monitor"],
            stdin=slave, stdout=slave, stderr=slave, close_fds=True, cwd=REPO,
        )
        os.close(slave)
        self._master = master
        self._chunks: list[bytes] = []
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self) -> None:
        while True:
            ready, _, _ = select.select([self._master], [], [], 1.0)
            if ready:
                try:
                    data = os.read(self._master, 4096)
                except OSError:
                    break
                if not data:
                    break
                with self._lock:
                    self._chunks.append(data)
            elif self._proc.poll() is not None:
                break

    def text(self) -> str:
        with self._lock:
            raw = b"".join(self._chunks)
        return raw.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")

    def wait_for(self, pattern: str, timeout_s: float, start: int = 0) -> int:
        """Index of @p pattern in the captured text, or -1 on timeout/exit."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            index = self.text().find(pattern, start)
            if index >= 0:
                return index
            if self._proc.poll() is not None:
                return -1
            time.sleep(0.5)
        return -1

    def stop(self) -> str:
        if self._proc.poll() is None:
            self._proc.terminate()
        self._proc.wait()
        self._thread.join(timeout=5)
        os.close(self._master)
        return self.text()


class Checker:
    def __init__(self, text: str) -> None:
        self.text = text
        self.failures = 0

    def expect(self, ok: bool, message: str) -> bool:
        print(("PASS: " if ok else "FAIL: ") + message)
        if not ok:
            self.failures += 1
        return ok

    def find(self, needle: str, start: int = 0) -> int:
        return self.text.find(needle, start)

    def find_re(self, pattern: str, start: int = 0) -> int:
        match = re.compile(pattern).search(self.text, start)
        return match.start() if match else -1

    def ordered(self, steps: list[tuple[str, str]], start: int = 0) -> int:
        """Assert regex steps appear in order; returns the last position."""
        pos = start
        for pattern, label in steps:
            found = self.find_re(pattern, pos)
            if not self.expect(found >= 0, f"{label} (/{pattern}/)"):
                return -1
            pos = found + 1
        return pos

    def no_crash(self) -> None:
        for pattern in CRASH_PATTERNS:
            self.expect(pattern not in self.text, f"no {pattern!r} in the log")


def firmware_line(version: str) -> str:
    return rf"Firmware: title={TITLE} version={re.escape(version)} "


def check_boot(c: Checker, version: str) -> None:
    c.ordered([
        (firmware_line(version), f"boots {version}"),
        (re.escape(READY) + rf": title={TITLE} version={re.escape(version)}",
         "firmware updater ready with the running version"),
        (r"\[ota\] firmware check requested", "firmware check requested"),
    ])
    c.no_crash()


def check_update(c: Checker, old: str, new: str) -> None:
    pos = c.ordered([
        (firmware_line(old), f"boots {old}"),
        (re.escape(READY), "firmware updater ready"),
        (rf"\[ota\] DOWNLOADING title={TITLE} version={re.escape(new)}",
         f"download of {new} starts"),
        (r"--ota-begin\(ota\)--> ota", "state machine enters OTA"),
        (r"\[ota\] progress \d+%", "download progress reported"),
        (r"\[ota\] image activated", "verified image activated"),
        (r"\[ota\] restarting into the new firmware", "restart announced"),
        (firmware_line(new), f"boots {new}"),
    ])
    if pos >= 0:
        # Sync (lamp apply) and updater init race after the reboot: no order.
        c.expect(c.find_re(re.escape(READY) + rf": title={TITLE} version={re.escape(new)} "
                           r"last_state=UPDATED", pos) >= 0, "new image reports UPDATED")
        c.expect(c.find_re(r"\[tb_app\] applied complete valid desired state", pos) >= 0,
                 "lamp state re-applied after the update")
    c.expect(c.find("[ota] FAILED") < 0, "no [ota] FAILED")
    if pos >= 0:
        c.expect(c.find_re(r"\[ota\] DOWNLOADING", pos) < 0,
                 f"{new} does not download again after the update")
    c.no_crash()


def check_failed(c: Checker, old: str, new: str) -> None:
    c.ordered([
        (firmware_line(old), f"boots {old}"),
        (rf"\[ota\] DOWNLOADING title={TITLE} version={re.escape(new)}",
         f"download of {new} starts"),
        (r"--ota-begin\(ota\)--> ota", "state machine enters OTA"),
        (r"\[ota\] FAILED: ", "failure reported"),
        (r"--ota-failed\(ota\)--> online", "state machine returns online"),
        (r"\[tb_app\] lamp output resumed", "lamp output resumed"),
    ])
    c.expect(c.find("[ota] restarting") < 0, "no restart after the failure")
    c.expect(len(re.findall(firmware_line(old), c.text)) == 1,
             "exactly one boot (no reboot)")
    c.no_crash()


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--expect", choices=["boot", "update", "failed"], required=True)
    parser.add_argument("--from-version", required=True)
    parser.add_argument("--to-version")
    parser.add_argument("--log", help="check an existing log instead of capturing")
    parser.add_argument("--port", default="/dev/ttyUSB1")
    parser.add_argument("--duration", type=int, default=90, help="monitor window (s)")
    parser.add_argument("--out", default="/tmp/klc_ota.log", help="captured log path")
    parser.add_argument("--deploy-file", help="image to deploy once the updater is ready")
    parser.add_argument("--device", default="klc-kitchen-99")
    parser.add_argument("--deploy-args", default="",
                        help="extra ota_deploy.py deploy args (e.g. '--allow-shared-profile' "
                             "or '--corrupt --version 1.0.1-bad')")
    return parser.parse_args(argv)


def capture(args: argparse.Namespace) -> str:
    monitor = MonitorCapture(args.port, args.duration)
    try:
        if args.deploy_file:
            if monitor.wait_for(READY, 120) < 0:
                print("FAIL: firmware updater never became ready; not deploying")
                return monitor.stop()
            time.sleep(5)
            cmd = [sys.executable, str(DEPLOY), "deploy", "--device", args.device,
                   "--file", args.deploy_file, "--wait", "0"]
            cmd += args.deploy_args.split()
            print("== deploying:", " ".join(cmd[1:]), flush=True)
            subprocess.check_call(cmd, cwd=REPO)
            if args.expect == "update":
                boot = monitor.wait_for(
                    f"Firmware: title={TITLE} version={args.to_version} ", args.duration)
                if boot >= 0:
                    monitor.wait_for("[tb_app] applied complete valid desired state", 90, boot)
            elif args.expect == "failed":
                failed = monitor.wait_for("[ota] FAILED", args.duration)
                if failed >= 0:
                    monitor.wait_for("[tb_app] lamp output resumed", 30, failed)
        else:
            monitor.wait_for("[ota] firmware check requested", args.duration)
        time.sleep(3)
    finally:
        text = monitor.stop()
    return text


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.expect != "boot" and not args.to_version:
        raise SystemExit("error: --to-version is required for update/failed")

    if args.log:
        text = Path(args.log).read_text(encoding="utf-8", errors="replace")
    else:
        text = capture(args)
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"== log written to {args.out}")

    checker = Checker(text)
    if args.expect == "boot":
        check_boot(checker, args.from_version)
    elif args.expect == "update":
        check_update(checker, args.from_version, args.to_version)
    else:
        check_failed(checker, args.from_version, args.to_version)

    print(f"== {checker.failures} failure(s)")
    return 0 if checker.failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
