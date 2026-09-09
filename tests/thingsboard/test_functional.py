"""Functional tests for the ThingsBoard test device over the real REST/HTTP API.

Opt-in only: requires TB_API_KEY and RUN_TB_FUNCTIONAL_TESTS=1, so these do
not run in ordinary host/unit test invocations or without network access.
Never asserts on or prints the API key or device access token.
"""
from __future__ import annotations

import os
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "thingsboard"))

from provision_test_device import DEFAULT_DEVICE_NAME, provision, token_path  # noqa: E402
from thingsboard_client import (  # noqa: E402
    ThingsBoardDeviceClient,
    ThingsBoardError,
    ThingsBoardRestClient,
)

BASE_URL = os.environ.get("TB_BASE_URL", "http://home-assistance.local:8080")
API_KEY = os.environ.get("TB_API_KEY")
RUN_FUNCTIONAL_TESTS = os.environ.get("RUN_TB_FUNCTIONAL_TESTS") == "1"


@unittest.skipUnless(
    RUN_FUNCTIONAL_TESTS and API_KEY,
    "set RUN_TB_FUNCTIONAL_TESTS=1 and TB_API_KEY to run against a real ThingsBoard tenant",
)
class ThingsBoardFunctionalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.rest_client = ThingsBoardRestClient(BASE_URL, API_KEY)
        cls.device_name = os.environ.get("TB_TEST_DEVICE_NAME", DEFAULT_DEVICE_NAME)
        cls.device_id, _ = provision(cls.rest_client, cls.device_name, "default")
        cls.access_token_path = token_path(cls.device_name)

    def test_provision_is_idempotent(self) -> None:
        device_id_again, _ = provision(self.rest_client, self.device_name, "default")
        self.assertEqual(device_id_again, self.device_id)

    def test_shared_attributes_roundtrip(self) -> None:
        self.rest_client.save_attributes(
            self.device_id, "SHARED_SCOPE", {"power": True, "brightness": 42}
        )
        values = self.rest_client.get_attributes(self.device_id, "SHARED_SCOPE", ["power", "brightness"])
        self.assertEqual(values.get("power"), True)
        self.assertEqual(values.get("brightness"), 42)

    def test_telemetry_publish_and_query(self) -> None:
        device_client = self._device_client()
        telemetry = {
            "power": True,
            "brightness": 77,
            "pwm_duty": 1965,
            "connection_state": "online",
            "fw_version": "0.0.0-test",
            "hardware": "esp32-wroom-32d",
            "uptime_ms": 12345,
        }
        device_client.post_telemetry(telemetry)
        time.sleep(1.0)  # allow ingestion before querying the same values back
        latest = self.rest_client.get_latest_timeseries(self.device_id, list(telemetry.keys()))
        for key, expected in telemetry.items():
            # timeseries values are returned as strings, including "true"/"false"
            expected_str = str(expected).lower() if isinstance(expected, bool) else str(expected)
            self.assertEqual(latest.get(key), expected_str, key)

    def test_rpc_two_way(self) -> None:
        device_client = self._device_client()
        expected_result = {"power": True, "brightness": 55}
        received: dict = {}

        def respond_once() -> None:
            request = device_client.poll_rpc(timeout_ms=15000)
            if request is None:
                return
            received["method"] = request.get("method")
            device_client.reply_rpc(request["id"], expected_result)

        poller = threading.Thread(target=respond_once, daemon=True)
        poller.start()
        time.sleep(0.5)  # let the long-poll subscribe before the server sends the RPC
        result = self.rest_client.send_rpc_twoway(self.device_id, "getState", {}, timeout_ms=10000)
        poller.join(timeout=5)

        self.assertEqual(received.get("method"), "getState")
        self.assertEqual(result, expected_result)

    def test_authentication_failure_is_reported(self) -> None:
        bad_client = ThingsBoardRestClient(BASE_URL, "not-a-real-api-key", timeout_s=5.0)
        with self.assertRaises(ThingsBoardError):
            bad_client.get_current_user()

    def test_unreachable_server_is_reported(self) -> None:
        bad_client = ThingsBoardRestClient(
            "http://tb-does-not-exist.invalid:8080", API_KEY, timeout_s=3.0
        )
        with self.assertRaises(ThingsBoardError):
            bad_client.get_current_user()

    def _device_client(self) -> ThingsBoardDeviceClient:
        token = self.access_token_path.read_text(encoding="utf-8").strip()
        return ThingsBoardDeviceClient(BASE_URL, token)


if __name__ == "__main__":
    unittest.main()
