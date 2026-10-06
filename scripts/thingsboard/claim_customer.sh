#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# claim_customer.sh — claim / reclaim a ThingsBoard device as a Customer User
#
# Customer-side half of the claiming toolset. The tenant prepares the device
# with `claim_handover.sh` (writes the one-time `claimingData` attribute); this
# script completes the transfer using only the Customer User's own credentials:
#
#   1. log in as a Customer User   POST /api/auth/login
#   2. claim the device            POST /api/customer/device/{deviceName}/claim
#                                  body: {"secretKey":"<one-time-secret>"}
#   3. reclaim (optional)          DELETE /api/customer/device/{deviceName}/claim
#
# The claim/reclaim calls require a Customer User JWT — the tenant API key is
# never accepted there. A test-environment helper (`setup-test-customer`)
# creates/reuses the test Customer + Customer User with the tenant API key
# (read at runtime from `TB_TENANT_API_KEY_FILE`), as described in the task's
# "Test environment" section.
#
# Usage
# -----
#   scripts/thingsboard/claim_customer.sh <device-name> [claim]
#       Claim <device-name> with the secret from TB_CLAIM_SECRET
#       (or --secret-stdin). `claim` is the default action.
#   scripts/thingsboard/claim_customer.sh <device-name> reclaim
#       Release the device back to the tenant (DELETE .../claim).
#   scripts/thingsboard/claim_customer.sh setup-test-customer
#       Test-environment helper: create/reuse the test Customer and Customer
#       User using the tenant API key (NOT the Customer User JWT).
#   scripts/thingsboard/claim_customer.sh -h | --help
#       Show this help.
#
# Options
# -------
#   --secret-stdin       Read the claiming secret from stdin (one line,
#                        not echoed) instead of TB_CLAIM_SECRET.
#   --customer-title T   Test Customer title (setup-test-customer only).
#   -h, --help           Show this help.
#
# Environment
# -----------
#   TB_BASE_URL            ThingsBoard base URL (default: http://home-assistance.local:8080)
#   TB_CUSTOMER_USERNAME   Customer User login (e-mail)              [required for claim/reclaim]
#   TB_CUSTOMER_PASSWORD   Customer User password                    [required for claim/reclaim]
#   TB_CLAIM_SECRET        One-time claiming secret                  [required for claim]
#   TB_CUSTOMER_TITLE      Test Customer title for setup-test-customer (default: KLC Test Customer)
#   TB_TENANT_API_KEY_FILE Tenant-admin API key file; used ONLY by setup-test-customer
#                          (default: /home/dima/projects/hq_workspace/thingboard_tenant_api_key)
#
# Exit codes
# ----------
#   0  success
#   1  unexpected error (network / REST / login / parse failure)
#   2  usage error
#   3  claim rejected: wrong secret                  ("... Secret key is incorrect!")
#   4  claim rejected: claiming data expired         ("... expired!")
#   5  claim rejected: device already claimed        (retry after a successful claim)
#   6  claim rejected: device not available for claiming (no claimingData)
#   7  claim rejected: other server-side reason (HTTP 400)
#
# Security
# --------
# The Customer User password, the claiming secret and the JWT are read at
# runtime and kept only in shell variables and non-filesystem channels
# (process-substitution pipes / anonymous pipes). They are never written to
# on-disk files, never printed or logged, and never placed in curl's process
# arguments (which are visible in process listings): request bodies are piped
# through process substitution (`<( ... )`) and the JWT/API-key header is fed
# to curl through a process-substitution config file (`-K <( ... )`) — both
# are anonymous pipes (`/dev/fd/*`), not files on disk. Because nothing
# sensitive touches the filesystem, there is no temporary file to clean up and
# no dependency on an EXIT trap for confidentiality. Response bodies are held
# only in the in-memory `RESP` shell variable. Status output never contains
# these values. Exception: in the setup-test-customer flow with
# TB_CUSTOMER_PASSWORD unset, the activation link (which contains the one-time
# activation token) is intentionally printed to stdout for the operator — treat
# it as a credential: do not log it, and do not capture stdout of
# setup-test-customer into a log file. The MQTT access token is NOT the
# claiming secret.
# ---------------------------------------------------------------------------
# Smoke tests (dev/test instance; smoke-test results are not proof of the
# target PE behavior — see docs/THINGSBOARD_USER_MANUAL.md §13.6):
#
#   # 1) tenant side: throwaway device + claimingData (see claim_handover.sh);
#   #    --handover prints ONLY the one-time secret to stdout — capture it in a
#   #    shell variable (memory only), never write it to a file:
#   SECRET_VAL="$(scripts/thingsboard/claim_handover.sh <device-name> --ttl 600 --handover 2>/dev/null)"
#   # 2) test Customer + Customer User (tenant API key; test env only)
#   export TB_CUSTOMER_USERNAME=<customer-user-email>
#   export TB_CUSTOMER_PASSWORD=<customer-user-password>
#   scripts/thingsboard/claim_customer.sh setup-test-customer
#   # 3) claim via --secret-stdin (non-persistent channel) / expected error
#   #    (retry) / reclaim — all with the Customer User JWT only
#   printf '%s' "$SECRET_VAL" | scripts/thingsboard/claim_customer.sh --secret-stdin <device-name> claim   # exit 0
#   printf '%s' "$SECRET_VAL" | scripts/thingsboard/claim_customer.sh --secret-stdin <device-name> claim   # exit 5: already claimed
#   scripts/thingsboard/claim_customer.sh <device-name> reclaim                                            # exit 0
#   # 4) wrong secret: (re-)issue claimingData (new secret, see claim_handover.sh)
#   scripts/thingsboard/claim_handover.sh <device-name> --ttl 600
#   export TB_CLAIM_SECRET=<deliberately-wrong-secret>          # placeholder: wrong on purpose
#   scripts/thingsboard/claim_customer.sh <device-name> claim   # exit 3
#   # expired claimingData: write claimingData with a past expirationTime via
#   # the tenant REST API, then claim  ->  exit 4
#   export TB_CLAIM_SECRET=<one-time-secret>                    # placeholder only (expiry rejected first)
#   scripts/thingsboard/claim_customer.sh <device-name> claim   # exit 4
#   unset TB_CLAIM_SECRET SECRET_VAL                           # memory-only values are gone with the shell
# ---------------------------------------------------------------------------
# Security: never allow inherited `bash -x` / xtrace to echo credentials or
# the JWT into stderr/logs. Disable xtrace before any sensitive value is read.
set +x
set -Eeuo pipefail
# Guard against a caller re-enabling tracing through the option flags.
set +o xtrace
umask 077

DEFAULT_TB_BASE_URL="http://home-assistance.local:8080"
DEFAULT_API_KEY_FILE="/home/dima/projects/hq_workspace/thingboard_tenant_api_key"
DEFAULT_CUSTOMER_TITLE="KLC Test Customer"

TB_BASE_URL="${TB_BASE_URL:-$DEFAULT_TB_BASE_URL}"
API_KEY_FILE="${TB_TENANT_API_KEY_FILE:-$DEFAULT_API_KEY_FILE}"
CUSTOMER_TITLE="${TB_CUSTOMER_TITLE:-$DEFAULT_CUSTOMER_TITLE}"

ACTION="claim"
SECRET_STDIN=0
DEVICE_NAME=""
# Sensitive values read at runtime; kept out of output/logs by redact().
TB_CUSTOMER_PASSWORD="${TB_CUSTOMER_PASSWORD:-}"
CLAIM_SECRET="${TB_CLAIM_SECRET:-}"
JWT=""
API_KEY=""
ACT_TOKEN=""
# Current auth header value ("X-Authorization: Bearer ..." or "... ApiKey ...").
AUTH_HEADER=""
# Result of the most recent REST call (memory only, never a file).
RESP=""
CODE=""

log() {
  printf '[claim_customer] %s\n' "$@" >&2
}

die() {
  local code=1
  if [[ "${1:-}" =~ ^[0-9]+$ ]]; then
    code="$1"
    shift
  fi
  log "ERROR: $*"
  exit "$code"
}

usage() {
  # Print the leading comment block (everything from line 2 up to the first
  # non-comment line), stripping the leading "# ".
  sed -n '2,/^[^#]/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

# Redact every sensitive value known to the script from a string that could
# end up in output or logs (error bodies, usage hints, ...).
redact() {
  local text="$1" value
  for value in \
    "$TB_CUSTOMER_PASSWORD" "$CLAIM_SECRET" "$JWT" "$API_KEY" "$ACT_TOKEN" "$AUTH_HEADER"; do
    if [[ -n "$value" ]]; then
      text="${text//"$value"/"***REDACTED***"}"
    fi
  done
  printf '%s' "$text"
}

# Short redacted response snippet for HTTP error messages (from $RESP).
http_error_hint() {
  local body
  body="$(printf '%s' "$RESP" | head -c 300 2>/dev/null || true)"
  redact "$body"
}

check_deps() {
  local tool
  for tool in curl jq sed tr head; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      die "required tool not found: $tool"
    fi
  done
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h | --help) usage; exit 0 ;;
      --secret-stdin) SECRET_STDIN=1; shift ;;
      --customer-title)
        if [[ $# -lt 2 ]]; then
          die 2 "--customer-title requires a value"
        fi
        CUSTOMER_TITLE="$2"
        shift 2
        ;;
      --customer-title=*) CUSTOMER_TITLE="${1#*=}"; shift ;;
      claim | reclaim | setup-test-customer) ACTION="$1"; shift ;;
      -*)
        die 2 "unknown option: $1 (see --help)"
        ;;
      *)
        if [[ -n "$DEVICE_NAME" ]]; then
          die 2 "unexpected extra argument: $1"
        fi
        DEVICE_NAME="$1"
        shift
        ;;
    esac
  done
}

validate_input() {
  if [[ "$ACTION" == "claim" || "$ACTION" == "reclaim" ]]; then
    if [[ -z "$DEVICE_NAME" ]]; then
      die 2 "device name is required for '$ACTION' (see --help)"
    fi
    if [[ "$DEVICE_NAME" =~ [[:cntrl:]] || "$DEVICE_NAME" =~ [[:space:]] ]]; then
      die 2 "device name must not contain whitespace or control characters"
    fi
    if [[ -z "$TB_CUSTOMER_USERNAME" || -z "$TB_CUSTOMER_PASSWORD" ]]; then
      die 2 "TB_CUSTOMER_USERNAME and TB_CUSTOMER_PASSWORD are required for '$ACTION'"
    fi
    if [[ "$TB_CUSTOMER_USERNAME" =~ [[:cntrl:]] || "$TB_CUSTOMER_USERNAME" =~ [[:space:]] ]]; then
      die 2 "TB_CUSTOMER_USERNAME must not contain whitespace or control characters"
    fi
  fi
  if [[ "$ACTION" == "claim" && "$SECRET_STDIN" != "1" && -z "$CLAIM_SECRET" ]]; then
    die 2 "TB_CLAIM_SECRET is required for 'claim' (or use --secret-stdin)"
  fi
  if [[ "$ACTION" == "setup-test-customer" ]]; then
    if [[ -z "$TB_CUSTOMER_USERNAME" ]]; then
      die 2 "TB_CUSTOMER_USERNAME (new Customer User e-mail) is required for 'setup-test-customer'"
    fi
    if [[ -n "$CUSTOMER_TITLE" && "$CUSTOMER_TITLE" =~ [[:cntrl:]] ]]; then
      die 2 "customer title must not contain control characters"
    fi
  fi
}

# URL-encode a path component (device names reach the URL path unquoted).
urlencode() {
  local s="$1" c encoded="" i
  for ((i = 0; i < ${#s}; i++)); do
    c="${s:i:1}"
    case "$c" in
      [A-Za-z0-9._~-]) encoded+="$c" ;;
      *) printf -v encoded '%s%%%02X' "$encoded" "'$c" ;;
    esac
  done
  printf '%s' "$encoded"
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

read_secret_stdin() {
  if [[ -t 0 ]]; then
    IFS= read -r -s -p "Claiming secret: " CLAIM_SECRET || true
    printf '\n' >&2
  else
    IFS= read -r CLAIM_SECRET || true
  fi
  if [[ -z "$CLAIM_SECRET" ]]; then
    die 2 "no claiming secret received from stdin"
  fi
}

# Perform a REST call keeping every credential on a non-filesystem channel.
#
#   $1  auth header value ("X-Authorization: Bearer ..." / "... ApiKey ..."),
#       or "" for anonymous calls (login / activation).
#   $@  remaining curl arguments. Request bodies must be supplied by the
#       caller via process substitution: `--data-binary @<( ... )`.
#
# The auth header is handed to curl through a process-substitution config file
# (`-K <( printf ... )`): that is an anonymous pipe (/dev/fd/*), never an
# on-disk file, so the JWT/API key is not persisted anywhere. The response body
# and the HTTP status code are stored in the in-memory globals $RESP / $CODE.
# tb_curl must be called in the current shell (NOT inside `$( ... )`) so those
# globals survive.
tb_curl() {
  local auth="$1"
  shift
  local out
  if [[ -n "$auth" ]]; then
    out="$(curl -sS -w $'\n%{http_code}' -K <(printf 'header = "%s"\n' "$auth") "$@")" \
      || die "REST call failed"
  else
    out="$(curl -sS -w $'\n%{http_code}' "$@")" || die "REST call failed"
  fi
  RESP="${out%$'\n'*}"
  CODE="${out##*$'\n'}"
}

# POST /api/auth/login as the Customer User. The password is piped (stdin) to
# jq; the JSON body reaches curl through a process-substitution pipe, so the
# password never reaches curl's process arguments or an on-disk file. The JWT
# is kept in $JWT / the derived $AUTH_HEADER (memory only).
tb_login() {
  local code
  tb_curl "" -X POST \
    -H "Content-Type: application/json" \
    --data-binary @<(printf '%s' "$TB_CUSTOMER_PASSWORD" |
      jq -Rc --arg user "$TB_CUSTOMER_USERNAME" '{username: $user, password: .}') \
    "$TB_BASE_URL/api/auth/login"
  code="$CODE"
  if [[ "$code" != "200" ]]; then
    die "Customer User login failed (HTTP $code): $(http_error_hint)"
  fi
  JWT="$(printf '%s' "$RESP" | jq -r '.token // empty')" || die "cannot parse login response"
  if [[ -z "$JWT" || "$JWT" == "null" ]]; then
    die "login response did not contain a token"
  fi
  AUTH_HEADER="X-Authorization: Bearer $JWT"
}

# Map a failed claim response (HTTP 400) to a documented expected-error exit
# code. Handles both the standard ThingsBoard error object ({"message": ...,
# "status": 400}) and the plain string bodies returned by the dev/test server
# ("CLAIMED" when the device is already claimed, "FAILURE" for other
# rejections). Never prints the secret.
classify_claim_rejection() {
  local msg lower raw
  raw="$(printf '%s' "$RESP" | tr -d '\r\n')"
  msg="$(printf '%s' "$RESP" | jq -r 'if type == "object" then (.message // empty) elif type == "string" then . else empty end' 2>/dev/null || true)"
  # Some instances return a bare (non-JSON) string body ("CLAIMED"/"FAILURE");
  # jq cannot parse it, so fall back to the raw body for classification.
  if [[ -z "$msg" ]]; then
    msg="$raw"
  fi
  lower="$(printf '%s' "$msg" | tr '[:upper:]' '[:lower:]')"
  case "$lower" in
    *"already claimed"* | claimed)
      log "claim rejected: device already claimed (exit 5)"
      exit 5
      ;;
    *"expired"*)
      log "claim rejected: claiming data expired (exit 4)"
      exit 4
      ;;
    *"secret key is incorrect"* | *"incorrect"*)
      log "claim rejected: wrong secret (exit 3)"
      exit 3
      ;;
    *"not available"*)
      log "claim rejected: device not available for claiming (exit 6)"
      exit 6
      ;;
    failure)
      log "claim rejected by the server (HTTP 400, no reason given) (exit 7)"
      exit 7
      ;;
    "")
      log "claim rejected (HTTP 400, empty response body) (exit 7)"
      exit 7
      ;;
    *)
      log "claim rejected: $(redact "$msg") (exit 7)"
      exit 7
      ;;
  esac
}

do_claim() {
  local code device_path
  device_path="$(urlencode "$DEVICE_NAME")"
  if [[ "$SECRET_STDIN" == "1" ]]; then
    read_secret_stdin
  fi
  # Trim accidental surrounding whitespace (e.g. a trailing newline when the
  # secret is taken from `TB_CLAIM_SECRET="$(cat file)"`).
  CLAIM_SECRET="$(printf '%s' "$CLAIM_SECRET" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
  if [[ -z "$CLAIM_SECRET" ]]; then
    die 2 "claim secret is empty"
  fi
  tb_curl "$AUTH_HEADER" -X POST \
    -H "Content-Type: application/json" \
    --data-binary @<(printf '%s' "$CLAIM_SECRET" | jq -Rc '{secretKey: .}') \
    "$TB_BASE_URL/api/customer/device/$device_path/claim"
  code="$CODE"
  if [[ "$code" =~ ^2 ]]; then
    printf 'claim: device %s claimed by customer user %s\n' "'$DEVICE_NAME'" "'$TB_CUSTOMER_USERNAME'"
    return 0
  fi
  if [[ "$code" == "400" ]]; then
    classify_claim_rejection
  fi
  die "claim failed (HTTP $code): $(http_error_hint)"
}

do_reclaim() {
  local code device_path
  device_path="$(urlencode "$DEVICE_NAME")"
  tb_curl "$AUTH_HEADER" -X DELETE \
    -H "Content-Type: application/json" \
    "$TB_BASE_URL/api/customer/device/$device_path/claim"
  code="$CODE"
  if [[ ! "$code" =~ ^2 ]]; then
    die "reclaim failed (HTTP $code): $(http_error_hint)"
  fi
  printf 'reclaim: device %s returned to the tenant by customer user %s\n' "'$DEVICE_NAME'" "'$TB_CUSTOMER_USERNAME'"
}

# Find a Customer by exact title. Returns the id on stdout, 1 when absent.
# The dev/test server exposes an exact `customerTitle` lookup; standard
# ThingsBoard needs the `textSearch` page form — both are tried.
tb_find_customer_id() {
  local title="$1" code id
  tb_curl "$AUTH_HEADER" -G \
    --data-urlencode "customerTitle=$title" \
    --data-urlencode "pageSize=1" --data-urlencode "page=0" \
    "$TB_BASE_URL/api/tenant/customers"
  code="$CODE"
  if [[ "$code" =~ ^2 ]]; then
    id="$(printf '%s' "$RESP" | jq -r --arg t "$title" '
      if (.title != null) then
        if (.title == $t and .id.id != null) then .id.id else empty end
      elif (.data | type) == "array" then
        (.data[] | select(.title == $t) | .id.id)
      else empty end' 2>/dev/null || true)"
    if [[ -n "$id" && "$id" != "null" ]]; then
      printf '%s' "$id"
      return 0
    fi
  elif [[ "$code" != "404" ]]; then
    log "customer exact-title lookup returned HTTP $code; trying text search"
  fi
  tb_curl "$AUTH_HEADER" -G \
    --data-urlencode "textSearch=$title" \
    --data-urlencode "pageSize=100" --data-urlencode "page=0" \
    "$TB_BASE_URL/api/tenant/customers"
  code="$CODE"
  if [[ "$code" =~ ^2 ]]; then
    id="$(printf '%s' "$RESP" | jq -r --arg t "$title" '(.data // empty) | .[]? | select(.title == $t) | .id.id' 2>/dev/null || true)"
    if [[ -n "$id" && "$id" != "null" ]]; then
      printf '%s' "$id"
      return 0
    fi
  elif [[ "$code" == "400" || "$code" == "500" ]]; then
    # Instances that require the exact `customerTitle` parameter (the
    # dev/test server) reject `textSearch` with 400/500; that only means "no
    # generic search — assume not found" (a duplicate create is caught by the
    # "already exists" handling in tb_create_customer).
    log "customer text search is not supported by this instance (HTTP $code); assuming the customer does not exist"
  else
    die "customer search failed (HTTP $code): $(http_error_hint)"
  fi
  return 1
}

tb_create_customer() {
  local title="$1" code id msg lower
  tb_curl "$AUTH_HEADER" -X POST \
    -H "Content-Type: application/json" \
    --data-binary @<(jq -nc --arg t "$title" '{title: $t}') \
    "$TB_BASE_URL/api/customer"
  code="$CODE"
  if [[ ! "$code" =~ ^2 ]]; then
    # A concurrent/identical title is an expected, recoverable situation:
    # reuse the existing customer.
    if [[ "$code" == "400" ]]; then
      msg="$(printf '%s' "$RESP" | jq -r '.message // empty' 2>/dev/null || true)"
      lower="$(printf '%s' "$msg" | tr '[:upper:]' '[:lower:]')"
      if [[ "$lower" == *"already exists"* ]]; then
        log "test customer '$title' already exists; reusing it"
        if id="$(tb_find_customer_id "$title")" && [[ -n "$id" ]]; then
          printf '%s' "$id"
          return 0
        fi
      fi
    fi
    die "creating test customer failed (HTTP $code): $(http_error_hint)"
  fi
  id="$(printf '%s' "$RESP" | jq -r '.id.id // empty')" || die "cannot parse customer response"
  if [[ -z "$id" || "$id" == "null" ]]; then
    die "customer response did not contain an id"
  fi
  printf '%s' "$id"
}

# Find the Customer User by e-mail within a Customer. Id on stdout, 1 when absent.
tb_find_customer_user_id() {
  local customer_id="$1" email="$2" code id
  tb_curl "$AUTH_HEADER" -G \
    --data-urlencode "pageSize=100" --data-urlencode "page=0" \
    "$TB_BASE_URL/api/customer/$customer_id/users"
  code="$CODE"
  if [[ ! "$code" =~ ^2 ]]; then
    die "listing customer users failed (HTTP $code): $(http_error_hint)"
  fi
  id="$(printf '%s' "$RESP" | jq -r --arg e "$email" '(.data // empty) | .[]? | select(.email == $e) | .id.id' 2>/dev/null || true)"
  if [[ -n "$id" && "$id" != "null" ]]; then
    printf '%s' "$id"
    return 0
  fi
  return 1
}

tb_create_customer_user() {
  local customer_id="$1" email="$2" code id
  tb_curl "$AUTH_HEADER" -X POST \
    -H "Content-Type: application/json" \
    --data-binary @<(jq -nc --arg e "$email" --arg cid "$customer_id" \
      '{email: $e, authority: "CUSTOMER_USER", customerId: {id: $cid, entityType: "CUSTOMER"}}') \
    "$TB_BASE_URL/api/user?sendActivationMail=false"
  code="$CODE"
  if [[ "$code" =~ ^2 ]]; then
    id="$(printf '%s' "$RESP" | jq -r '.id.id // empty')" || die "cannot parse customer user response"
    if [[ -n "$id" && "$id" != "null" ]]; then
      printf '%s' "$id"
      return 0
    fi
    die "customer user response did not contain an id"
  fi
  # An existing e-mail is an expected, recoverable situation: the caller then
  # looks the user up again.
  if [[ "$code" == "400" ]]; then
    local msg
    msg="$(printf '%s' "$RESP" | jq -r '.message // empty' 2>/dev/null || true)"
    if [[ "$(printf '%s' "$msg" | tr '[:upper:]' '[:lower:]')" == *"already present"* ]]; then
      log "customer user '$email' already exists; reusing it"
      return 1
    fi
  fi
  die "creating customer user failed (HTTP $code): $(http_error_hint)"
}

# Activate a freshly created (or re-issued) Customer User. On the dev/test
# server, an already-activated user refuses a new activation link (HTTP 400
# "User is already activated!") — that is treated as success: the operator
# keeps the existing password.
# With TB_CUSTOMER_PASSWORD set, the activation token is consumed here
# (programmatic activation). Without it, the activation link (which contains
# the one-time activation token) is intentionally printed to stdout for the
# operator — the caller must not log it (see the header's Security section).
tb_activate_customer_user() {
  local user_id="$1" code link token msg lower
  tb_curl "$AUTH_HEADER" "$TB_BASE_URL/api/user/$user_id/activationLink"
  code="$CODE"
  if [[ "$code" =~ ^2 ]]; then
    link="$(printf '%s' "$RESP" | tr -d '[:space:]')"
    if [[ "$link" != http* ]]; then
      link="$(printf '%s' "$RESP" | jq -r 'if type == "string" then . elif type == "object" then (.activationLink // empty) else empty end' 2>/dev/null || true)"
    fi
    if [[ -z "$link" ]]; then
      die "activation link response could not be parsed"
    fi
    if [[ -z "$TB_CUSTOMER_PASSWORD" ]]; then
      printf '%s\n' "$link"
      log "test user not activated yet; pass the printed activation link to the operator"
      return 0
    fi
    token="$(printf '%s' "$link" | sed 's/.*activateToken=//')"
    ACT_TOKEN="$token"
    tb_curl "" -X POST \
      -H "Content-Type: application/json" \
      --data-binary @<(printf '%s' "$TB_CUSTOMER_PASSWORD" |
        jq -Rc --arg t "$token" '{activateToken: $t, password: .}') \
      "$TB_BASE_URL/api/noauth/activate"
    code="$CODE"
    ACT_TOKEN=""
    if [[ ! "$code" =~ ^2 ]]; then
      die "activating the test user failed (HTTP $code): $(http_error_hint)"
    fi
    log "test user activated (password set)"
    return 0
  fi
  msg="$(printf '%s' "$RESP" | jq -r '.message // empty' 2>/dev/null || true)"
  lower="$(printf '%s' "$msg" | tr '[:upper:]' '[:lower:]')"
  if [[ "$code" == "400" && "$lower" == *"already activated"* ]]; then
    log "test user already activated; the existing password stays in effect"
    return 0
  fi
  die "getting/activating the activation link failed (HTTP $code): $(http_error_hint)"
}

# Test-environment helper: create/reuse the test Customer + Customer User.
# Uses the tenant API key from TB_TENANT_API_KEY_FILE (never the key value in
# output), NOT the Customer User JWT.
do_setup_test_customer() {
  local api_key customer_id user_id link_output
  api_key="$(read_api_key)"
  API_KEY="$api_key"
  AUTH_HEADER="X-Authorization: ApiKey $api_key"
  api_key=""

  if ! customer_id="$(tb_find_customer_id "$CUSTOMER_TITLE")"; then
    log "test customer '$CUSTOMER_TITLE' not found; creating it"
    customer_id="$(tb_create_customer "$CUSTOMER_TITLE")"
  else
    log "test customer '$CUSTOMER_TITLE' found (id $customer_id)"
  fi

  if ! user_id="$(tb_find_customer_user_id "$customer_id" "$TB_CUSTOMER_USERNAME")"; then
    log "customer user '$TB_CUSTOMER_USERNAME' not found; creating it"
    if ! user_id="$(tb_create_customer_user "$customer_id" "$TB_CUSTOMER_USERNAME")"; then
      # Expected when the e-mail already existed: look it up after creation.
      user_id="$(tb_find_customer_user_id "$customer_id" "$TB_CUSTOMER_USERNAME")" \
        || die "cannot resolve customer user '$TB_CUSTOMER_USERNAME' after creation"
    fi
  else
    log "customer user '$TB_CUSTOMER_USERNAME' found (id $user_id)"
  fi

  link_output="$(tb_activate_customer_user "$user_id")"
  if [[ -n "$link_output" ]]; then
    # stdout is reserved for the activation link; the summary goes to stderr.
    printf '%s\n' "$link_output"
    log "activation link printed above — open it in a browser to set the password (or re-run with TB_CUSTOMER_PASSWORD set to activate programmatically)"
  else
    printf 'setup-test-customer: customer %s, user %s ready\n' "'$CUSTOMER_TITLE'" "'$TB_CUSTOMER_USERNAME'"
  fi
}

main() {
  check_deps
  parse_args "$@"
  validate_input

  # No temporary files are created: every credential (password, claiming
  # secret, JWT/API key) travels only through shell variables and
  # process-substitution pipes, and response bodies are held in $RESP.
  case "$ACTION" in
    setup-test-customer)
      do_setup_test_customer
      ;;
    *)
      tb_login
      case "$ACTION" in
        claim) do_claim ;;
        reclaim) do_reclaim ;;
      esac
      ;;
  esac
}

main "$@"