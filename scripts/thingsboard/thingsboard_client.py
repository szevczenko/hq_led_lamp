"""Minimal ThingsBoard REST/HTTP clients using only the Python standard library.

Two clients are provided:

- `ThingsBoardRestClient` — tenant-scoped REST API (authenticated with the
  tenant API key). See `../../Server-side-api` for the endpoint reference.
- `ThingsBoardDeviceClient` — device-scoped HTTP API (authenticated with a
  per-device access token), used to act as a mock device for functional
  tests: https://thingsboard.io/docs/reference/http-api/

Never log or print an API key or device access token. Callers are
responsible for keeping those values out of committed files and test output.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
import uuid
from typing import Any, Mapping, Optional, Sequence

DEFAULT_TIMEOUT_S = 10.0


class ThingsBoardError(RuntimeError):
    """Raised for authentication, network, or unexpected-response failures."""


def _request(
    base_url: str,
    method: str,
    path: str,
    *,
    headers: Mapping[str, str],
    query: Optional[Mapping[str, Any]] = None,
    body: Any = None,
    raw_body: Optional[bytes] = None,
    content_type: Optional[str] = None,
    timeout_s: float = DEFAULT_TIMEOUT_S,
) -> Any:
    url = f"{base_url.rstrip('/')}{path}"
    if query:
        url = f"{url}?{urllib.parse.urlencode(query)}"
    data = raw_body if raw_body is not None else (
        json.dumps(body).encode("utf-8") if body is not None else None)
    request = urllib.request.Request(url, data=data, method=method)
    for key, value in headers.items():
        request.add_header(key, value)
    if data is not None:
        request.add_header("Content-Type", content_type or "application/json")
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            raw = response.read()
            if not raw:
                return None
            return json.loads(raw)
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        exc.close()
        raise ThingsBoardError(f"{method} {path} failed: HTTP {exc.code} {detail}") from None
    except urllib.error.URLError as exc:
        raise ThingsBoardError(f"{method} {path} unreachable: {exc.reason}") from None
    except TimeoutError as exc:
        raise ThingsBoardError(f"{method} {path} timed out") from exc


class ThingsBoardRestClient:
    """Tenant-scoped REST client authenticated with a tenant API key."""

    def __init__(self, base_url: str, api_key: str, timeout_s: float = DEFAULT_TIMEOUT_S) -> None:
        self._base_url = base_url
        self._api_key = api_key
        self._timeout_s = timeout_s

    def _call(self, method: str, path: str, *, query=None, body=None, timeout_s=None,
              raw_body=None, content_type=None) -> Any:
        return _request(
            self._base_url,
            method,
            path,
            headers={"X-Authorization": f"ApiKey {self._api_key}"},
            query=query,
            body=body,
            raw_body=raw_body,
            content_type=content_type,
            timeout_s=timeout_s if timeout_s is not None else self._timeout_s,
        )

    def get_current_user(self) -> dict:
        return self._call("GET", "/api/auth/user")

    def find_device_by_name(self, name: str) -> Optional[dict]:
        try:
            return self._call("GET", "/api/tenant/devices", query={"deviceName": name})
        except ThingsBoardError as exc:
            if "HTTP 404" in str(exc):
                return None
            raise

    def create_device(self, name: str, device_profile_name: str = "default") -> dict:
        return self._call("POST", "/api/device", body={"name": name, "type": device_profile_name})

    def get_device_credentials(self, device_id: str) -> dict:
        return self._call("GET", f"/api/device/{device_id}/credentials")

    def delete_device(self, device_id: str) -> None:
        self._call("DELETE", f"/api/device/{device_id}")

    def save_attributes(self, device_id: str, scope: str, attributes: Mapping[str, Any]) -> None:
        self._call(
            "POST",
            f"/api/plugins/telemetry/DEVICE/{device_id}/attributes/{scope}",
            body=dict(attributes),
        )

    def get_attributes(self, device_id: str, scope: str, keys: Sequence[str]) -> dict:
        entries = self._call(
            "GET",
            f"/api/plugins/telemetry/DEVICE/{device_id}/values/attributes/{scope}",
            query={"keys": ",".join(keys)},
        )
        return {entry["key"]: entry["value"] for entry in entries or []}

    def get_latest_timeseries(self, device_id: str, keys: Sequence[str]) -> dict:
        series = self._call(
            "GET",
            f"/api/plugins/telemetry/DEVICE/{device_id}/values/timeseries",
            query={"keys": ",".join(keys)},
        )
        return {key: points[0]["value"] for key, points in (series or {}).items() if points}

    def get_latest_timeseries_with_ts(self, device_id: str, keys: Sequence[str]) -> dict:
        """Returns {key: (ts_ms, value)} for keys that have a latest value."""
        series = self._call(
            "GET",
            f"/api/plugins/telemetry/DEVICE/{device_id}/values/timeseries",
            query={"keys": ",".join(keys)},
        )
        return {
            key: (int(points[0]["ts"]), points[0]["value"])
            for key, points in (series or {}).items()
            if points and points[0].get("value") is not None
        }

    def send_rpc_oneway(self, device_id: str, method: str, params: Mapping[str, Any]) -> None:
        self._call(
            "POST",
            f"/api/rpc/oneway/{device_id}",
            body={"method": method, "params": dict(params)},
        )

    def send_rpc_twoway(
        self, device_id: str, method: str, params: Mapping[str, Any], timeout_ms: int = 10000
    ) -> Any:
        return self._call(
            "POST",
            f"/api/rpc/twoway/{device_id}",
            body={"method": method, "params": dict(params), "timeout": timeout_ms},
            timeout_s=(timeout_ms / 1000.0) + 5.0,
        )

    # --- Device profiles / OTA packages -------------------------------

    def get_device_profile(self, profile_id: str) -> dict:
        return self._call("GET", f"/api/deviceProfile/{profile_id}")

    def save_device_profile(self, profile: Mapping[str, Any]) -> dict:
        return self._call("POST", "/api/deviceProfile", body=dict(profile))

    def list_devices_by_profile_name(self, profile_name: str, page_size: int = 100) -> list:
        page = self._call(
            "GET",
            "/api/tenant/devices",
            query={"pageSize": page_size, "page": 0, "type": profile_name},
        )
        return (page or {}).get("data", [])

    def list_ota_packages(self, page_size: int = 100) -> list:
        page = self._call("GET", "/api/otaPackages", query={"pageSize": page_size, "page": 0})
        return (page or {}).get("data", [])

    def create_ota_package(self, title: str, version: str, device_profile_id: str,
                           package_type: str = "FIRMWARE") -> dict:
        return self._call(
            "POST",
            "/api/otaPackage",
            body={
                "title": title,
                "version": version,
                "type": package_type,
                "deviceProfileId": {"entityType": "DEVICE_PROFILE", "id": device_profile_id},
                "isURL": False,
            },
        )

    def upload_ota_package_data(self, package_id: str, file_name: str, data: bytes,
                                checksum_algorithm: str = "SHA256") -> dict:
        boundary = f"----klc{uuid.uuid4().hex}"
        body = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{file_name}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode("utf-8") + data + f"\r\n--{boundary}--\r\n".encode("utf-8")
        return self._call(
            "POST",
            f"/api/otaPackage/{package_id}",
            query={"checksumAlgorithm": checksum_algorithm},
            raw_body=body,
            content_type=f"multipart/form-data; boundary={boundary}",
            timeout_s=120.0,
        )

    def delete_ota_package(self, package_id: str) -> None:
        self._call("DELETE", f"/api/otaPackage/{package_id}")


class ThingsBoardDeviceClient:
    """Device-scoped HTTP client authenticated with a per-device access token.

    Used only to simulate a device in functional tests; production firmware
    uses the MQTT/TLS transport documented in the production plan.
    """

    def __init__(self, base_url: str, access_token: str, timeout_s: float = DEFAULT_TIMEOUT_S) -> None:
        self._base_url = base_url
        self._access_token = access_token
        self._timeout_s = timeout_s

    def _call(self, method: str, path: str, *, query=None, body=None, timeout_s=None) -> Any:
        return _request(
            self._base_url,
            method,
            f"/api/v1/{self._access_token}{path}",
            headers={},
            query=query,
            body=body,
            timeout_s=timeout_s if timeout_s is not None else self._timeout_s,
        )

    def post_telemetry(self, payload: Mapping[str, Any]) -> None:
        self._call("POST", "/telemetry", body=dict(payload))

    def get_attributes(self, shared_keys: Sequence[str]) -> dict:
        result = self._call("GET", "/attributes", query={"sharedKeys": ",".join(shared_keys)})
        return (result or {}).get("shared", {})

    def poll_rpc(self, timeout_ms: int = 20000) -> Optional[dict]:
        """Long-poll for one server-side RPC request; returns None on timeout."""
        try:
            return self._call(
                "GET",
                "/rpc",
                query={"timeout": timeout_ms},
                timeout_s=(timeout_ms / 1000.0) + 5.0,
            )
        except ThingsBoardError as exc:
            if "HTTP 408" in str(exc):
                return None
            raise

    def reply_rpc(self, request_id: int, result: Mapping[str, Any]) -> None:
        self._call("POST", f"/rpc/{request_id}", body=dict(result))
