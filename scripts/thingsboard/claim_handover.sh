#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# claim_handover.sh — prepare a ThingsBoard device for customer claiming
#
# Tenant-side / production helper that publishes a one-time `claimingData`
# SERVER_SCOPE attribute for a given device name, exactly as described in
# `FIRMWARE_PRODUCTION_READINESS_PLAN.md` §9.5:
#
#   POST /api/plugins/telemetry/DEVICE/{deviceId}/attributes/SERVER_SCOPE
#   {"claimingData":"{\"secretKey\":\"<secret>\",\"expirationTime\":<ms>}"}
#
# The Customer User completes the transfer out-of-band with
# `POST /api/customer/device/{deviceName}/claim` (TASK-303 / the PE claiming
# endpoints). This script only prepares the device server-side.
#
# Usage
# -----
#   scripts/thingsboard/claim_handover.sh <device-name> [--ttl SECONDS]
#       Issue (or re-issue: overwrite) claimingData with a fresh secret.
#       This is the default action.
#   scripts/thingsboard/claim_handover.sh <device-name> --reissue
#       Explicit re-issuance: requires claimingData to already exist, then
#       overwrites it with a new secret.
#   scripts/thingsboard/claim_handover.sh <device-name> --invalidate
#       Invalidate: delete the claimingData attribute (idempotent).
#   scripts/thingsboard/claim_handover.sh <device-name> --status
#       Show whether claimingData is set and its expiration — never the secret.
#   scripts/thingsboard/claim_handover.sh <device-name> --handover
#       Issue and print ONLY the secret to stdout (explicit handover mode, e.g.
#       to pipe into the out-of-band channel: waybill / label / e-mail).
#
# Options
# ------
#   --ttl SECONDS    Claim validity in seconds (default: env TB_CLAIM_TTL or 3600)
#   -h, --help       Show this help
#
# Environment
# -----------
#   TB_BASE_URL            ThingsBoard base URL (default: http://home-assistance.local:8080)
#   TB_TENANT_API_KEY_FILE Tenant-admin API key file (default:
#                          /home/dima/projects/hq_workspace/thingboard_tenant_api_key)
#   TB_CLAIM_TTL           Claim validity in seconds (default: 3600)
#
# The tenant-admin authorization is read at runtime from the key file. The key
# and the claiming secret are never placed in curl process arguments (which are
# visible in process listings): the key is carried in a private (0600) curl
# config file and the request body in a private (0600) request file, both
# created at runtime and removed on exit. Neither value is printed or logged:
# only operation status (without the secret) is emitted, and the secret is
# printed to stdout solely in explicit --handover mode. The MQTT access token
# is NOT the claiming secret.
#
# Smoke test (local dev instance; CE/test-env results are a smoke test only,
# they are not proof of the target PE behavior — see THINGSBOARD_USER_MANUAL.md
# §13.6):
# ---------------------------------------------------------------------------
#   # 1) create a throwaway device (idempotent in this example)
#   KEY=$(tr -d '[:space:]' < /home/dima/projects/hq_workspace/thingboard_tenant_api_key)
#   curl -sS -X POST "http://home-assistance.local:8080/api/device" \
#     -H "Content-Type: application/json" \
#     -H "X-Authorization: ApiKey $KEY" \
#     -d '{"name":"klc-claim-smoke-test","type":"default"}' >/dev/null
#   # 2) issue, verify, re-issue, invalidate
#   scripts/thingsboard/claim_handover.sh klc-claim-smoke-test --ttl 600
#   scripts/thingsboard/claim_handover.sh klc-claim-smoke-test --status
#   scripts/thingsboard/claim_handover.sh klc-claim-smoke-test --reissue
#   scripts/thingsboard/claim_handover.sh klc-claim-smoke-test --invalidate
#   scripts/thingsboard/claim_handover.sh klc-claim-smoke-test --status
#   # 3) handover mode: prints the secret to stdout only
#   scripts/thingsboard/claim_handover.sh klc-claim-smoke-test --handover > /tmp/klc-claim-smoke.secret
#   # 4) delete the throwaway device
#   DEV_ID=$(curl -sS -G -H "Content-Type: application/json" \
#     -H "X-Authorization: ApiKey $KEY" \
#     --data-urlencode "deviceName=klc-claim-smoke-test" \
#     "http://home-assistance.local:8080/api/tenant/devices" | jq -r .id.id)
#   curl -sS -X DELETE -H "X-Authorization: ApiKey $KEY" \
#     "http://home-assistance.local:8080/api/device/$DEV_ID" >/dev/null
# ---------------------------------------------------------------------------
# Security: never allow inherited `bash -x` / xtrace to echo the tenant API key
# or the generated claiming secret into stderr/logs. Disable xtrace before any
# sensitive value is read or generated (and before the request options below).
set +x
set -Eeuo pipefail
# Guard against a caller re-enabling tracing through the option flags.
set +o xtrace
umask 077

DEFAULT_TB_BASE_URL="http://home-assistance.local:8080"
DEFAULT_API_KEY_FILE="/home/dima/projects/hq_workspace/thingboard_tenant_api_key"
DEFAULT_TTL=3600
CLAIM_KEY="claimingData"
CLAIM_SCOPE="SERVER_SCOPE"

TB_BASE_URL="${TB_BASE_URL:-$DEFAULT_TB_BASE_URL}"
API_KEY_FILE="${TB_TENANT_API_KEY_FILE:-$DEFAULT_API_KEY_FILE}"
TTL="${TB_CLAIM_TTL:-$DEFAULT_TTL}"

ACTION="issue"
HANDOVER=0
DEVICE_NAME=""
CLAIM_SECRET=""

log() {
  printf '[claim_handover] %s\n' "$@" >&2
}

die() {
  log "ERROR: $*"
  exit 1
}

usage() {
  sed -n '2,51p' "$0" | sed 's/^# \{0,1\}//'
}

# Redact the tenant API key and the claiming secret from any string that could
# end up in output or logs (error bodies, debug hints, ...).
redact() {
  local text="$1"
  if [[ -n "${api_key:-}" ]]; then
    text="${text//"$api_key"/"***REDACTED***"}"
  fi
  if [[ -n "${CLAIM_SECRET:-}" ]]; then
    text="${text//"$CLAIM_SECRET"/"***REDACTED***"}"
  fi
  printf '%s' "$text"
}

# Short redacted body snippet for HTTP error messages.
http_error_hint() {
  local body
  body="$(head -c 300 "$tmp_resp" 2>/dev/null || true)"
  redact "$body"
}

read_api_key() {
  if [[ ! -r "$API_KEY_FILE" ]]; then
    die "tenant API key file is not readable: $API_KEY_FILE"
  fi
  local key
  key="$(tr -d '[:space:]' <"$API_KEY_FILE")" || die "cannot read tenant API key file: $API_KEY_FILE"
  if [[ -z "$key" ]]; then
    die "tenant API key file is empty: $API_KEY_FILE"
  fi
  printf '%s' "$key"
}

check_deps() {
  local tool
  for tool in curl jq openssl tr date sed; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      die "required tool not found: $tool"
    fi
  done
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h | --help) usage; exit 0 ;;
      --issue) ACTION="issue"; shift ;;
      --reissue) ACTION="reissue"; shift ;;
      --invalidate) ACTION="invalidate"; shift ;;
      --status) ACTION="status"; shift ;;
      --handover) HANDOVER=1; shift ;;
      --ttl)
        if [[ $# -lt 2 ]]; then
          die "--ttl requires a value in seconds"
        fi
        TTL="$2"
        shift 2
        ;;
      --ttl=*) TTL="${1#*=}"; shift ;;
      -*)
        die "unknown option: $1 (see --help)"
        ;;
      *)
        if [[ -n "$DEVICE_NAME" ]]; then
          die "unexpected extra argument: $1"
        fi
        DEVICE_NAME="$1"
        shift
        ;;
    esac
  done
}

validate_input() {
  if [[ -z "$DEVICE_NAME" ]]; then
    die "device name is required (see --help)"
  fi
  if [[ "$DEVICE_NAME" =~ [[:cntrl:]] || "$DEVICE_NAME" =~ [[:space:]] ]]; then
    die "device name must not contain whitespace or control characters"
  fi
  if [[ ! "$TTL" =~ ^[0-9]+$ || "$TTL" -eq 0 ]]; then
    die "--ttl / TB_CLAIM_TTL must be a positive integer (seconds)"
  fi
  if [[ "$HANDOVER" == "1" && "$ACTION" != "issue" && "$ACTION" != "reissue" ]]; then
    die "--handover can only be combined with issuing (default / --reissue)"
  fi
}

# Resolve a device name to its ThingsBoard device id. Prints the id on stdout;
# exits with status 1 when the device does not exist.
tb_find_device() {
  local device_name="$1"
  local code id
  code="$(curl -sS -o "$tmp_resp" -w '%{http_code}' \
    -G -H "Content-Type: application/json" -K "$curl_cfg" \
    --data-urlencode "deviceName=$device_name" \
    "$TB_BASE_URL/api/tenant/devices")" || die "REST call to resolve device '$device_name' failed"
  if [[ "$code" == "404" ]]; then
    return 1
  fi
  if [[ ! "$code" =~ ^2 ]]; then
    die "device lookup failed (HTTP $code): $(http_error_hint)"
  fi
  id="$(jq -r '
    if (.id.id != null) then .id.id
    elif ((.data | type) == "array" and (.data | length) > 0) then .data[0].id.id
    else empty end' "$tmp_resp")" || die "cannot parse device lookup response"
  if [[ -z "$id" || "$id" == "null" ]]; then
    log "device '$device_name' not found (no matching device entity)"
    return 1
  fi
  printf '%s' "$id"
}

# Read the current claimingData attribute value (inner JSON as a string) and
# store it in the global CLAIMING_DATA_VALUE. Returns 0 when the attribute is
# present, 1 when it is absent. REST and parse failures are hard errors (die):
# callers must not report them as "absent".
tb_get_claiming_data() {
  local device_id="$1"
  local code value
  CLAIMING_DATA_VALUE=""
  code="$(curl -sS -o "$tmp_resp" -w '%{http_code}' \
    -G -H "Content-Type: application/json" -K "$curl_cfg" \
    --data-urlencode "keys=$CLAIM_KEY" \
    "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$device_id/values/attributes/$CLAIM_SCOPE")" \
    || die "REST call to read claimingData failed"
  if [[ ! "$code" =~ ^2 ]]; then
    die "reading claimingData failed (HTTP $code): $(http_error_hint)"
  fi
  value="$(jq -r --arg key "$CLAIM_KEY" '.[] | select(.key == $key) | .value // empty' "$tmp_resp")" \
    || die "cannot parse attribute response"
  if [[ -z "$value" || "$value" == "null" ]]; then
    return 1
  fi
  CLAIMING_DATA_VALUE="$value"
}

# Write claimingData (create or overwrite). The secret is fed to jq on stdin
# and the final payload goes to a private request file, so no secret ever
# appears in jq's or curl's process arguments.
tb_save_claiming_data() {
  local device_id="$1" expiration_ms="$2"
  local payload code
  payload="$(printf '%s' "$CLAIM_SECRET" |
    jq -Rc --argjson expirationTime "$expiration_ms" \
      '{claimingData: ({secretKey: ., expirationTime: $expirationTime} | tojson)}')" \
    || die "cannot build claimingData payload"
  printf '%s' "$payload" >"$payload_file"
  code="$(curl -sS -o "$tmp_resp" -w '%{http_code}' -X POST \
    -H "Content-Type: application/json" -K "$curl_cfg" \
    --data-binary "@$payload_file" \
    "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$device_id/attributes/$CLAIM_SCOPE")" \
    || die "REST call to write claimingData failed"
  if [[ ! "$code" =~ ^2 ]]; then
    die "writing claimingData failed (HTTP $code): $(http_error_hint)"
  fi
}

# Delete claimingData (idempotent). Tries the documented per-key endpoint and
# falls back to the legacy `?keys=` form on instances that do not expose it.
tb_delete_claiming_data() {
  local device_id="$1"
  local code
  if ! tb_get_claiming_data "$device_id"; then
    return 0
  fi
  code="$(curl -sS -o "$tmp_resp" -w '%{http_code}' -X DELETE \
    -H "Content-Type: application/json" -K "$curl_cfg" \
    "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$device_id/$CLAIM_SCOPE/$CLAIM_KEY")" \
    || die "REST call to delete claimingData failed"
  if [[ "$code" == "404" ]]; then
    log "per-key delete endpoint not available (HTTP 404); using legacy ?keys= form"
    code="$(curl -sS -o "$tmp_resp" -w '%{http_code}' -X DELETE -G \
      -H "Content-Type: application/json" -K "$curl_cfg" \
      --data-urlencode "keys=$CLAIM_KEY" \
      "$TB_BASE_URL/api/plugins/telemetry/DEVICE/$device_id/$CLAIM_SCOPE")" \
      || die "REST call to delete claimingData (legacy endpoint) failed"
  fi
  if [[ ! "$code" =~ ^2 ]]; then
    die "invalidating claimingData failed (HTTP $code): $(http_error_hint)"
  fi
  if tb_get_claiming_data "$device_id"; then
    die "claimingData is still present after delete (HTTP $code)"
  fi
}

format_expiration() {
  local expiration_ms="$1"
  local exp_s now_s
  exp_s=$((expiration_ms / 1000))
  now_s="$(date +%s)"
  printf 'expires %s UTC (%s seconds remaining)' \
    "$(date -u -d "@$exp_s" '+%Y-%m-%dT%H:%M:%SZ')" "$((exp_s - now_s))"
}

do_issue() {
  local device_id="$1"
  local existing=""
  if tb_get_claiming_data "$device_id"; then
    existing="1"
  fi
  if [[ "$ACTION" == "reissue" && -z "$existing" ]]; then
    die "nothing to re-issue: claimingData is not set for device '$DEVICE_NAME'"
  fi
  CLAIM_SECRET="$(openssl rand -hex 32)" || die "cannot generate a crypto-random secret"
  expiration_ms=$((($(date +%s) + TTL) * 1000))
  tb_save_claiming_data "$device_id" "$expiration_ms"
  if [[ -n "$existing" ]]; then
    log "claimingData re-issued for device '$DEVICE_NAME' (device id $device_id): $(format_expiration "$expiration_ms")"
  else
    log "claimingData issued for device '$DEVICE_NAME' (device id $device_id): $(format_expiration "$expiration_ms")"
  fi
  if [[ "$HANDOVER" == "1" ]]; then
    printf '%s\n' "$CLAIM_SECRET"
  else
    printf 'claimingData: %s for device %s\n' "${existing:+re-}issued" "'$DEVICE_NAME'"
  fi
}

do_invalidate() {
  local device_id="$1"
  if ! tb_get_claiming_data "$device_id"; then
    printf "claimingData: absent for device %s; nothing to invalidate\n" "'$DEVICE_NAME'"
    return 0
  fi
  tb_delete_claiming_data "$device_id"
  printf "claimingData: invalidated for device %s\n" "'$DEVICE_NAME'"
}

do_status() {
  local device_id="$1"
  local exp_ms value
  # tb_get_claiming_data returns 1 only when the attribute is genuinely absent;
  # REST and parse failures die here instead of being misreported as absent.
  if ! tb_get_claiming_data "$device_id"; then
    printf "claimingData: absent for device %s\n" "'$DEVICE_NAME'"
    return 0
  fi
  value="$CLAIMING_DATA_VALUE"
  exp_ms="$(printf '%s' "$value" | jq -r '.expirationTime // empty')" \
    || die "cannot parse claimingData expirationTime"
  if [[ -z "$exp_ms" || "$exp_ms" == "null" ]]; then
    printf "claimingData: present for device %s (expiration not parseable)\n" "'$DEVICE_NAME'"
    return 0
  fi
  printf "claimingData: present for device %s (%s)\n" "'$DEVICE_NAME'" "$(format_expiration "$exp_ms")"
}

main() {
  check_deps
  parse_args "$@"
  validate_input

  local api_key device_id
  api_key="$(read_api_key)" || exit 1

  tmp_resp="$(mktemp "${TMPDIR:-/tmp}/claim_handover.XXXXXX")" || die "cannot create a temporary file"
  # Private (0600) curl config and request-body files: the tenant API key only
  # ever lives in the config file and the claim payload (with the secret) only
  # in the request file, so neither appears in curl's process arguments.
  curl_cfg="$(mktemp "${TMPDIR:-/tmp}/claim_handover_curl_cfg.XXXXXX")" || die "cannot create a temporary curl config file"
  payload_file="$(mktemp "${TMPDIR:-/tmp}/claim_handover_payload.XXXXXX")" || die "cannot create a temporary payload file"
  chmod 600 "$curl_cfg" "$payload_file"
  printf 'header = "X-Authorization: ApiKey %s"\n' "$api_key" >"$curl_cfg"
  trap 'rm -f "$tmp_resp" "$curl_cfg" "$payload_file"' EXIT

  if ! device_id="$(tb_find_device "$DEVICE_NAME")"; then
    die "device '$DEVICE_NAME' was not found on $TB_BASE_URL"
  fi

  case "$ACTION" in
    issue | reissue) do_issue "$device_id" ;;
    invalidate) do_invalidate "$device_id" ;;
    status) do_status "$device_id" ;;
    *) die "internal error: unknown action '$ACTION'" ;;
  esac
}

main "$@"