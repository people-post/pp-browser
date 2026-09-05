#!/usr/bin/env bash
# L0: HTTP smoke against a running pp-node (image or binary).
# Does not prove relay forwarding — only that the node is up and caps started.
#
# Usage:
#   ./scripts/test/pp_node_image_smoke.sh
#   PP_NODE_STATUS_URL=http://127.0.0.1:18518 ./scripts/test/pp_node_image_smoke.sh
#   ./scripts/test/pp_node_image_smoke.sh --expect-circuit=0 --expect-media=0
#
# See packaging/pp-node/IMAGE_SMOKE.md
set -euo pipefail

STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
EXPECT_CIRCUIT="${PP_NODE_EXPECT_CIRCUIT:-1}"
EXPECT_MEDIA="${PP_NODE_EXPECT_MEDIA:-1}"
STATUS_TOKEN="${PP_NODE_STATUS_TOKEN:-}"
TIMEOUT_SEC="${PP_NODE_SMOKE_TIMEOUT_SEC:-30}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

  --status-url URL     Default: \$PP_NODE_STATUS_URL or http://127.0.0.1:18518
  --expect-circuit 0|1 Expect status.circuit_relay (default 1)
  --expect-media 0|1   Expect status.media_relay (default 1)
  --token TOKEN        Bearer for PP_NODE_STATUS_TOKEN
  --timeout SEC        Wait for /healthz (default 30)
  -h, --help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --expect-circuit) EXPECT_CIRCUIT="$2"; shift 2 ;;
    --expect-media) EXPECT_MEDIA="$2"; shift 2 ;;
    --token) STATUS_TOKEN="$2"; shift 2 ;;
    --timeout) TIMEOUT_SEC="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "error: missing command: $1" >&2
    exit 1
  }
}

need_cmd curl
need_cmd python3

auth_args=()
if [[ -n "${STATUS_TOKEN}" ]]; then
  auth_args=(-H "Authorization: Bearer ${STATUS_TOKEN}")
fi

curl_get() {
  local path="$1"
  curl -fsS -m 5 "${auth_args[@]}" "${STATUS_URL}${path}"
}

echo "pp-node L0 smoke → ${STATUS_URL}"

deadline=$((SECONDS + TIMEOUT_SEC))
health=""
while (( SECONDS < deadline )); do
  if health="$(curl_get /healthz 2>/dev/null)"; then
    break
  fi
  sleep 0.5
done
if [[ -z "${health}" ]]; then
  echo "error: /healthz not ready within ${TIMEOUT_SEC}s" >&2
  exit 1
fi

python3 - "$health" <<'PY'
import json, sys
h = json.loads(sys.argv[1])
if not h.get("ok") or not h.get("host_running"):
    raise SystemExit(f"healthz failed: {h}")
print("ok  /healthz", h)
PY

status="$(curl_get /status)"
export PP_NODE_SMOKE_STATUS_JSON="${status}"
export PP_NODE_SMOKE_EXPECT_CIRCUIT="${EXPECT_CIRCUIT}"
export PP_NODE_SMOKE_EXPECT_MEDIA="${EXPECT_MEDIA}"

python3 <<'PY'
import json, os, sys

s = json.loads(os.environ["PP_NODE_SMOKE_STATUS_JSON"])
expect_c = os.environ["PP_NODE_SMOKE_EXPECT_CIRCUIT"] == "1"
expect_m = os.environ["PP_NODE_SMOKE_EXPECT_MEDIA"] == "1"

def fail(msg: str) -> None:
    raise SystemExit(f"error: {msg}: {s}")

if not s.get("host_running"):
    fail("host_running false")
peer = s.get("peer_id") or ""
if not peer:
    fail("missing peer_id")
listen = s.get("listen") or ""
if not listen:
    fail("missing listen")
if "/udp/" not in listen or "/adp/" not in listen:
    fail("listen is not Amp ADP (/udp/…/adp/…)")

c = bool(s.get("circuit_relay"))
m = bool(s.get("media_relay"))
if c != expect_c:
    fail(f"circuit_relay={c} expected {expect_c}")
if m != expect_m:
    fail(f"media_relay={m} expected {expect_m}")

print(f"ok  /status peer_id={peer}")
print(f"ok  listen={listen}")
print(f"ok  circuit_relay={c} media_relay={m}")
PY

# Optional auth negative check when token configured
if [[ -n "${STATUS_TOKEN}" ]]; then
  code="$(curl -sS -o /dev/null -w '%{http_code}' -m 5 "${STATUS_URL}/healthz" || true)"
  if [[ "${code}" != "401" ]]; then
    echo "error: expected 401 without Bearer, got ${code}" >&2
    exit 1
  fi
  echo "ok  status auth rejects missing Bearer"
fi

echo "pp-node L0 smoke PASSED"
