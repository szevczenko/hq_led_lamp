#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Automated on-target provisioning smoke check (TASK-128).

The provisioning portal is an on-target, radio-level feature: a phone or
laptop joins the device's temporary access point, is captured by the
captive DNS responder, and submits the home SSID/password through the HTTP
portal.  Host mocks cannot verify it end to end; this script is the
automated verification step of the standard on-target flash/monitor loop.

It captures the ESP-IDF monitor log (either by running the flash/monitor
loop itself or from an already-captured log file) and asserts:

  (a) no ``Backtrace:`` appears — the log is backtrace-free,
  (b) the provisioning state transitions observed in the log match the
      legal state-machine sequence (app_state.h: NETWORK
      --PROVISIONING_STARTED--> PROVISIONING --PROVISIONING_SUCCEEDED/FAILED->
      NETWORK | SAFE_OFF), including the bounded-retry re-entry the
      state machine legally performs after a provisioning failure,
  (c) on a fresh-device boot (``--expect provisioning``) the TASK-135
      portal-reachability signature ``[INFO]: [prov_mgr] provisioning AP up;
      portal reachable`` was observed — the WHOLE portal (radio AP+STA plus
      the bound HTTP/captive-DNS listeners) is up, not merely the radio,
  (d) no SSID or password substring of the test credential appears
      anywhere in the captured log — the secrecy rule holds even for the
      credential the manual procedure submits,
  (e) on a stale-credential boot (``--expect stale-credential`` /
      ``--expect stale-recovery``, TASK-139) the bounded fallback is
      observed: one or more ``NETWORK_FAILED`` episodes up to the
      configured ``--fallback-budget``, then PROVISIONING_STARTED ->
      PROVISIONING with the portal reachable, exactly one ENTER and one
      portal-up period (no restart storm), and — for ``stale-recovery`` —
      the full recovery PROVISIONING_SUCCEEDED -> NETWORK -> TLS after a
      human re-provisions through the portal.

The test credential is a documented, throwaway value (the manual procedure
in tests/wifi_provisioning_manager/README.md uses it on a lab/throwaway
WLAN).  It can be overridden with --test-ssid/--test-password or the
KLC_SMOKE_TEST_SSID/KLC_SMOKE_TEST_PASSWORD environment variables so the
no-secret-leak assertion mirrors the exact credential a lab actually uses.

Operation modes
---------------
* offline validation of an already captured log::

      python3 tests/wifi_provisioning_manager/on_target_smoke_check.py \
          --log /tmp/hq_led_lamp_esp.log

* full flash/monitor loop (default; flash + bounded monitor)::

      python3 tests/wifi_provisioning_manager/on_target_smoke_check.py \
          --port /dev/ttyUSB1 --monitor-duration 60 \
          --expect provisioning

Exit status is 0 when every assertion passes and non-zero otherwise.
"""

import argparse
import os
import re
import subprocess
import sys

# --------------------------------------------------------------------- #
# Documented test credential (throwaway values, see the component README) #
# --------------------------------------------------------------------- #

DEFAULT_TEST_SSID = "KLC-Smoke-128-Test"
DEFAULT_TEST_PASSWORD = "KLC-Sm0ke-Pa55-128!"

# Documented throwaway STALE credential (TASK-139): the deliberately wrong
# saved credential seeded into the storage image (never the real home
# secrets).  The no-secret-leak assertion checks BOTH the stale pair and the
# submitted (test) pair.
DEFAULT_STALE_SSID = "KLC-Stale-Net-139"
DEFAULT_STALE_PASSWORD = "KLC-Stale-Pass-139!"

# Product fallback budget (sdkconfig.defaults):
# CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS=2 — the maximum number of
# NETWORK_FAILED episodes (one CONNECT_FAILED each) the device may go through
# before the portal must open.  Overridable with --fallback-budget to mirror
# a different build.
DEFAULT_FALLBACK_BUDGET = 2

# Minimum substring length the no-secret-leak assertion checks.  Shorter
# slices of a credential are too generic to prove a leak; the full
# credential is always checked regardless of length.
_MIN_SUBSTRING_LEN = 6

# --------------------------------------------------------------------- #
# Provisioning marker table                                              #
#                                                                        #
# The markers are credential-free substrings emitted by the product boot  #
# flow (main/app_main.c, tag "klc") and by the provisioning adapter       #
# (components/wifi_provisioning_manager, osal log "[prov_mgr]").  They    #
# are the documented log signatures for the controller-driven flow        #
# (TASK-131/132/133): the platform fallback controller owns the portal;   #
# the supervisor (TASK-133) only DELIVERS its outcome events, so the      #
# observable markers are the STARTED/SUCCEEDED/FAILED deliveries, the     #
# SAFE_OFF park line and the adapter's explicit-stop line (OTA/FATAL).    #
# --------------------------------------------------------------------- #

MARKER_TABLE = (
    # Bounded-retry NETWORK episode end (TASK-139): the app_state machine
    # reports `network --network-failed(network)--> safe-off` when the saved
    # credential did not connect within the gate window.  On a stale
    # credential each episode corresponds to one CONNECT_FAILED the platform
    # fallback controller counts against its budget — at most
    # CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS episodes happen before
    # the portal opens (one per bounded-retry NETWORK session).
    ("NETWORK_FAILED", "network --network-failed(network)--> safe-off"),
    # Provisioning entry (NETWORK --PROVISIONING_STARTED--> PROVISIONING):
    # the supervisor delivering the controller's fallback STARTED event.
    ("ENTER", "Provisioning flow started by the controller; entering Wi-Fi provisioning"),
    # Provisioning success: the controller retired the portal; the machine
    # returns to the NETWORK gate (-> TLS).
    ("SUCCESS", "Provisioning succeeded; portal retired by the controller"),
    # Provisioning failure: the machine parks degraded (bounded retry).
    ("FAILED", "Provisioning failed; machine parks degraded"),
    # Explicit adapter stop (OTA entry / FATAL): controller lifecycle ended,
    # portal listeners closed.
    ("PORTAL_STOPPED", "[prov_mgr] provisioning portal stopped"),
    # The machine parked degraded (SAFE_OFF) waiting for provisioning/reset/OTA.
    ("PARK", "waiting for provisioning / reset / OTA"),
    # Recovery after re-provisioning (TASK-139 MANUAL STEP): the NETWORK gate
    # passes (station connected with the submitted credential) and the
    # verified-TLS gate establishes the ThingsBoard transport.
    ("NETWORK", "Network connected; verified MQTT/TLS connect is now allowed"),
    ("TLS", "Verified TLS connected with validated identity"),
)

# State machine (normative view of app_state.h, provisioning stage only).
#
#   START        - no provisioning marker seen yet (may be a credentialed
#                  pass-through boot, or a stale-credential device looping
#                  through bounded-retry NETWORK_FAILED episodes)
#   ENTERED      - PROVISIONING_STARTED delivered; machine in PROVISIONING
#                  while the controller keeps the portal up
#   SUCCEEDED    - PROVISIONING_SUCCEEDED observed; the machine resumes the
#                  NETWORK gate with the freshly stored credential
#   NETWORK_UP   - the resumed NETWORK gate passed (station connected)
#   TLS_REACHED  - the verified-TLS gate established the transport (terminal
#                  for the recovery path)
#   FAILED_PARK  - PROVISIONING_FAILED observed; machine parked (bounded
#                  retry); a later fallback cycle may legally re-enter
#   RETIRED      - explicit adapter stop (OTA entry / FATAL) ended the
#                  controller lifecycle; no further provisioning activity
#
# Every recognized marker not listed for the current state is an illegal
# transition and fails the check.  On a successful provisioning the portal
# is retired by the controller without an adapter stop line, so SUCCESS is
# not followed by PORTAL_STOPPED in the normal boot.
TRANSITIONS = {
    "START": {
        # Bounded-retry NETWORK episodes (stale credential, TASK-139): the
        # machine parks SAFE_OFF and re-enters NETWORK until the fallback
        # controller's CONNECT_FAILED budget opens the portal (ENTER).
        "NETWORK_FAILED": "START",
        "ENTER": "ENTERED",
        "SUCCESS": None,
        "FAILED": None,
        # Tolerated in START: a credentialed pass-through device may park in
        # SAFE_OFF (e.g. a later TLS failure) without ever entering
        # provisioning for this boot episode.
        "PORTAL_STOPPED": "RETIRED",
        "PARK": "START",
        "NETWORK": None,
        "TLS": None,
    },
    "ENTERED": {
        "ENTER": None,
        "SUCCESS": "SUCCEEDED",
        "FAILED": "FAILED_PARK",
        "PORTAL_STOPPED": "RETIRED",
        "PARK": None,
        "NETWORK_FAILED": None,
        "NETWORK": None,
        "TLS": None,
    },
    "SUCCEEDED": {
        # Provisioning is complete: the machine resumes the NETWORK gate
        # with the submitted credential and proceeds to the TLS gate
        # (TASK-139 recovery).  A later PARK (a post-TLS failure) and a
        # duplicate adapter stop line are tolerated.  A fresh ENTER or a
        # second outcome after SUCCESS is illegal for this boot episode.
        "ENTER": None,
        "SUCCESS": None,
        "FAILED": None,
        "PORTAL_STOPPED": "SUCCEEDED",
        "PARK": "SUCCEEDED",
        "NETWORK_FAILED": None,
        "NETWORK": "NETWORK_UP",
        "TLS": None,
    },
    "NETWORK_UP": {
        "TLS": "TLS_REACHED",
        "ENTER": None,
        "SUCCESS": None,
        "FAILED": None,
        "PORTAL_STOPPED": "NETWORK_UP",
        "PARK": "NETWORK_UP",
        "NETWORK_FAILED": None,
        "NETWORK": None,
    },
    "TLS_REACHED": {
        # Terminal for the recovery path: the device reached the
        # verified-TLS gate after re-provisioning.  A post-TLS park and a
        # duplicate adapter stop line are tolerated.
        "ENTER": None,
        "SUCCESS": None,
        "FAILED": None,
        "PORTAL_STOPPED": "TLS_REACHED",
        "PARK": "TLS_REACHED",
        "NETWORK_FAILED": None,
        "NETWORK": None,
        "TLS": None,
    },
    "FAILED_PARK": {
        "ENTER": "ENTERED",  # bounded retry / next fallback cycle legally re-enters
        "SUCCESS": None,
        "FAILED": None,
        "PORTAL_STOPPED": "FAILED_PARK",
        "PARK": "FAILED_PARK",
        "NETWORK_FAILED": None,
        "NETWORK": None,
        "TLS": None,
    },
    "RETIRED": {
        "ENTER": None,
        "SUCCESS": None,
        "FAILED": None,
        "PORTAL_STOPPED": "RETIRED",
        "PARK": "RETIRED",
        "NETWORK_FAILED": None,
        "NETWORK": None,
        "TLS": None,
    },
}

# Marker(s) whose occurrence proves the boot actually entered the
# provisioning stage on a fresh device.
_EXPECT_PROVISIONING_MARKERS = ("ENTER",)

# Needle of the ENTER marker (stable reference for error messages; it is no
# longer MARKER_TABLE[0] since TASK-139 prepended NETWORK_FAILED).
_ENTER_NEEDLE = "Provisioning flow started by the controller; entering Wi-Fi provisioning"

# TASK-135 portal-reachability signature: the adapter emits ONE
# credential-free line per portal-up period when the WHOLE portal is up —
# the radio reached AP+STA **and** both owned listeners (HTTP + captive
# DNS) are bound, i.e. exactly the platform reachability surface
# `wifi_http_provisioning_is_reachable()`.  A fresh-device boot must show it;
# it is not part of the transition state machine because it can legally
# repeat across fallback re-entries (each portal-up false->true edge).
PORTAL_REACHABLE_SIGNATURE = "provisioning AP up; portal reachable"

# Boot banner that proves the monitor captured a real boot (sanity check
# against an empty/quiet serial line).
BOOT_BANNER = "Kitchen LED Controller starting"


def _env(flag_value, env_name):
    """Return --flag value, else $ENV_NAME, else None."""
    if flag_value is not None:
        return flag_value
    return os.environ.get(env_name)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="On-target provisioning smoke check (TASK-128).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--log",
        metavar="FILE",
        default=None,
        help="Validate an already captured monitor log instead of running "
             "the flash/monitor loop.  Use '-' to read the log from stdin.",
    )
    parser.add_argument(
        "--port",
        default="/dev/ttyUSB1",
        help="Serial port for the flash/monitor loop.",
    )
    parser.add_argument(
        "--monitor-duration",
        type=int,
        default=60,
        help="Bounded monitor window in seconds (matches the standard "
             "'timeout 1m idf.py monitor' loop).",
    )
    parser.add_argument(
        "--flash",
        action="store_true",
        default=True,
        help="Run 'idf.py flash' before monitoring (default).",
    )
    parser.add_argument(
        "--no-flash",
        dest="flash",
        action="store_false",
        help="Skip 'idf.py flash' and only monitor.",
    )
    parser.add_argument(
        "--test-ssid",
        default=None,
        help="Test SSID whose substrings must NOT appear in the log "
             "(default: %s, or $KLC_SMOKE_TEST_SSID)."
             % DEFAULT_TEST_SSID,
    )
    parser.add_argument(
        "--test-password",
        default=None,
        help="Test password whose substrings must NOT appear in the log "
             "(default: %s, or $KLC_SMOKE_TEST_PASSWORD)."
             % DEFAULT_TEST_PASSWORD,
    )
    parser.add_argument(
        "--stale-ssid",
        default=None,
        help="Stale (deliberately wrong) SSID whose substrings must NOT "
             "appear in the log (default: %s, or $KLC_SMOKE_STALE_SSID). "
             "Checked on TASK-139 stale-credential runs together with the "
             "submitted test credential." % DEFAULT_STALE_SSID,
    )
    parser.add_argument(
        "--stale-password",
        default=None,
        help="Stale (deliberately wrong) password whose substrings must NOT "
             "appear in the log (default: %s, or $KLC_SMOKE_STALE_PASSWORD)."
             % DEFAULT_STALE_PASSWORD,
    )
    parser.add_argument(
        "--fallback-budget",
        type=int,
        default=DEFAULT_FALLBACK_BUDGET,
        help="Configured CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS "
             "budget (consecutive NETWORK_FAILED episodes a stale-credential "
             "device may go through before the portal must open).",
    )
    parser.add_argument(
        "--expect",
        choices=("provisioning", "pass-through", "any",
                 "stale-credential", "stale-recovery"),
        default="provisioning",
        help="Boot branch the test expects: 'provisioning' (fresh device "
             "must enter the portal AND show the TASK-135 portal-reachable "
             "signature), 'pass-through' (credentialed device must skip the "
             "portal), 'any' (validate whatever legal branch occurs), "
             "'stale-credential' (TASK-139: a device with a wrong saved "
             "credential must go through at most the configured "
             "FALLBACK_ATTEMPTS NETWORK_FAILED episodes and then enter the "
             "portal with no restart storm), or 'stale-recovery' (the same "
             "bounded fallback PLUS the full recovery after re-provisioning: "
             "PROVISIONING_SUCCEEDED -> NETWORK -> TLS in the same log).",
    )
    return parser.parse_args(argv)


def run_capture(port, duration, flash):
    """Run the flash/monitor loop and return the captured log text."""
    if flash:
        subprocess.check_call(
            ["idf.py", "flash", "-p", port], stdout=sys.stdout, stderr=sys.stderr
        )
    try:
        proc = subprocess.run(
            ["timeout", str(duration), "idf.py", "-p", port, "monitor"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except FileNotFoundError as exc:
        sys.stderr.write("error: cannot run idf.py: %s\n" % exc)
        sys.exit(2)
    # The monitor is intentionally killed at the end of the bounded window;
    # GNU timeout reports 124, and idf.py monitor may forward a non-zero
    # exit once the serial session is torn down.  A real idf.py failure
    # (bad port, missing tool) produces no captured output instead.
    if proc.returncode not in (0, 124) and not proc.stdout:
        sys.stderr.write(
            "error: flash/monitor loop failed (rc=%d)\n" % proc.returncode
        )
        sys.exit(proc.returncode)
    text = proc.stdout.decode("utf-8", errors="replace")
    if not text:
        sys.stderr.write("error: monitor captured no output\n")
        sys.exit(1)
    return text


def load_log(args):
    """Return the monitor log text for the selected operation mode."""
    if args.log and args.log != "-":
        with open(args.log, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    if args.log == "-":
        return sys.stdin.read()
    return run_capture(args.port, args.monitor_duration, args.flash)


def find_marker_sequence(text):
    """Return the ordered, consecutive-duplicate-free marker sequence."""
    sequence = []
    for line in text.splitlines():
        for marker, needle in MARKER_TABLE:
            if needle in line:
                if not sequence or sequence[-1] != marker:
                    sequence.append(marker)
                break
    return sequence


def validate_sequence(sequence, expect):
    """Validate the marker sequence against the legal state machine.

    Returns (ok, detail) where ok is False on an illegal transition.
    """
    state = "START"
    for marker in sequence:
        nxt = TRANSITIONS[state][marker]
        if nxt is None:
            return False, (
                "illegal provisioning transition: marker '%s' observed in "
                "state '%s'" % (marker, state)
            )
        state = nxt

    if expect in ("provisioning", "stale-credential", "stale-recovery"):
        if "ENTER" not in sequence:
            return False, (
                "expected a boot entering Wi-Fi provisioning, "
                "but no '%s' marker was observed" % _ENTER_NEEDLE
            )
    elif expect == "pass-through":
        if "ENTER" in sequence:
            return False, (
                "expected a credentialed pass-through boot (no provisioning "
                "entry), but the '%s' marker was observed" % _ENTER_NEEDLE
            )
    entered = "ENTER" in sequence
    recovered = state in ("NETWORK_UP", "TLS_REACHED")
    if entered:
        branch = "stale-credential-fallback" if "NETWORK_FAILED" in sequence \
            else "provisioning"
    else:
        branch = "pass-through"
    if recovered:
        branch += " -> recovery"
    return True, "legal %s sequence%s" % (
        branch,
        ": " + " -> ".join(sequence) if sequence else "",
    )


def credential_substrings(value, min_len=_MIN_SUBSTRING_LEN):
    """All substrings of ``value`` with length >= min_len."""
    value = value or ""
    subs = set()
    length = len(value)
    if length == 0:
        return subs
    if length < min_len:
        subs.add(value)
        return subs
    for start in range(length):
        for end in range(start + min_len, length + 1):
            subs.add(value[start:end])
    return subs


def find_secret_leak(text, credential_pairs):
    """Return the first leaked credential substring, or None.

    ``credential_pairs`` is an iterable of ``(label, value)`` tuples; every
    substring (length >= _MIN_SUBSTRING_LEN) of every value is looked up in
    ``text``.  TASK-139 checks BOTH the submitted test credential and the
    deliberately wrong stale saved credential.
    """
    for name, value in credential_pairs:
        for substring in sorted(credential_substrings(value), key=len, reverse=True):
            if substring in text:
                return "%s substring %r" % (name, substring)
    return None


def main(argv=None):
    args = parse_args(argv)

    test_ssid = _env(args.test_ssid, "KLC_SMOKE_TEST_SSID") or DEFAULT_TEST_SSID
    test_password = (
        _env(args.test_password, "KLC_SMOKE_TEST_PASSWORD") or DEFAULT_TEST_PASSWORD
    )
    stale_ssid = _env(args.stale_ssid, "KLC_SMOKE_STALE_SSID") or DEFAULT_STALE_SSID
    stale_password = (
        _env(args.stale_password, "KLC_SMOKE_STALE_PASSWORD")
        or DEFAULT_STALE_PASSWORD
    )

    text = load_log(args)
    lines = text.splitlines()

    failures = []

    # (a) backtrace-free log ------------------------------------------- #
    backtrace_lines = [ln for ln in lines if "Backtrace:" in ln]
    if backtrace_lines:
        failures.append(
            "log contains %d 'Backtrace:' line(s) (first: %r)"
            % (len(backtrace_lines), backtrace_lines[0].strip())
        )

    # Sanity: the monitor must have captured a real boot.
    if BOOT_BANNER not in text and not any(needle in text for _, needle in MARKER_TABLE):
        failures.append(
            "log contains neither the boot banner (%r) nor any recognized "
            "provisioning marker; is the serial port correct?" % BOOT_BANNER
        )

    # (b) legal state-machine transition sequence ---------------------- #
    sequence = find_marker_sequence(text)
    ok, detail = validate_sequence(sequence, args.expect)
    print("provisioning transitions: %s" % detail)
    if not ok:
        failures.append("state-machine sequence check failed: %s" % detail)

    # (c) TASK-135 portal-reachability signature on a fresh-device boot - #
    # The WHOLE portal (radio AP+STA + both bound listeners) must be up,  #
    # not merely the radio: the signature fires once per portal-up        #
    # false->true edge (deduplicated, re-armed by stop).  A fresh-device  #
    # boot reaches it right after the controller's STARTED event; a       #
    # credentialed pass-through boot intentionally never shows it, so the #
    # assertion applies only to --expect provisioning.                    #
    entered = "ENTER" in sequence
    if (args.expect == "provisioning" or entered) and \
            PORTAL_REACHABLE_SIGNATURE not in text:
        failures.append(
            "expected a fresh-device boot with a reachable provisioning "
            "portal, but the TASK-135 signature %r was not observed"
            % ("[INFO]: [prov_mgr] " + PORTAL_REACHABLE_SIGNATURE)
        )

    # (c2) TASK-139 stale-credential bounded fallback -------------------- #
    # A device with a wrong saved credential must NOT park forever with no
    # AP: the platform fallback controller opens the portal after at most
    # --fallback-budget NETWORK_FAILED episodes (one CONNECT_FAILED each),
    # and the portal must then stay up — exactly one ENTER and exactly one
    # portal-up period (no restart storm, no repeated start/stop cycles).
    if args.expect in ("stale-credential", "stale-recovery"):
        enter_positions = [i for i, m in enumerate(sequence) if m == "ENTER"]
        if not enter_positions:
            failures.append(
                "expected the stale-credential device to enter Wi-Fi "
                "provisioning after the bounded fallback, but no '%s' "
                "marker was observed" % _ENTER_NEEDLE
            )
        else:
            nf_before = sequence[:enter_positions[0]].count("NETWORK_FAILED")
            if not (1 <= nf_before <= args.fallback_budget):
                failures.append(
                    "bounded fallback: observed %d NETWORK_FAILED episode(s) "
                    "before PROVISIONING_STARTED; expected 1..%d (the "
                    "configured CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS)"
                    % (nf_before, args.fallback_budget)
                )
        if sequence.count("ENTER") != 1:
            failures.append(
                "portal restart storm: the device entered provisioning %d "
                "time(s); expected exactly one ENTER (no restart storm)"
                % sequence.count("ENTER")
            )
        portal_up_count = text.count("provisioning AP up; portal reachable")
        if portal_up_count != 1:
            failures.append(
                "portal restart storm: the TASK-135 portal-reachability "
                "signature appeared %d time(s); expected exactly one "
                "portal-up period" % portal_up_count
            )
        if args.expect == "stale-recovery":
            for marker, name in (
                ("SUCCESS", "PROVISIONING_SUCCEEDED (portal retired after grace)"),
                ("NETWORK", "NETWORK gate pass (station connected)"),
                ("TLS", "verified-TLS gate reach"),
            ):
                if marker not in sequence:
                    failures.append(
                        "stale-recovery: expected the full recovery (%s) "
                        "after re-provisioning in the captured log, but it "
                        "was not observed" % name
                    )

    # (d) no credential content in the log ------------------------------ #
    # TASK-139: BOTH the deliberately wrong stale saved credential and the
    # (correct) submitted test credential must stay out of the log.
    leak = find_secret_leak(
        text,
        (
            ("test SSID", test_ssid),
            ("test password", test_password),
            ("stale SSID", stale_ssid),
            ("stale password", stale_password),
        ),
    )
    if leak is not None:
        failures.append(
            "credential content leaked into the log: %s" % leak
        )

    if failures:
        for failure in failures:
            sys.stderr.write("FAIL: %s\n" % failure)
        sys.stderr.write("\nCaptured log: %d lines.\n" % len(lines))
        return 1

    verb = "offline log"
    if args.log and args.log != "-":
        verb = "log file %s" % args.log
    elif args.log == "-":
        verb = "log on stdin"
    else:
        verb = "flash/monitor loop on %s" % args.port
    entered = "ENTER" in sequence
    portal = PORTAL_REACHABLE_SIGNATURE in text
    recovered = sequence[-1] in ("NETWORK_UP", "TLS_REACHED") \
        if sequence else False
    stale_note = ""
    if args.expect in ("stale-credential", "stale-recovery"):
        stale_note = ", bounded stale-credential fallback with no restart " \
                     "storm"
    if recovered:
        stale_note += ", full recovery reached " + sequence[-1]
    print(
        "PASS: %s — backtrace-free, legal %s transition sequence%s, "
        "TASK-135 portal-reachability signature %s, no stale/test "
        "credential substring in %d log lines."
        % (
            verb,
            "stale-credential-fallback" if entered and "NETWORK_FAILED"
            in sequence else ("provisioning" if entered else "pass-through"),
            stale_note,
            "observed" if portal else "not applicable (pass-through)",
            len(lines),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())