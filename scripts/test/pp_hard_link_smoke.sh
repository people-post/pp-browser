#!/usr/bin/env bash
# Wave 2 hard-lab link profiles: N-HARD-LOSSY / N-HARD-ASYM / N-HARD-BW.
#
# Prefer: ./scripts/test/pp_local_test.sh run --suite hard-w2
# See packaging/pp-node/HARD_LAB.md and docs/ops/TEST_STRATEGY.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_hard_lab_lib.sh
source "${ROOT}/scripts/test/pp_hard_lab_lib.sh"

PROFILE=""
SKIP_UP=0
WITH_CHAT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile) PROFILE="$2"; shift 2 ;;
    --status-url) PP_HARD_STATUS_URL="$2"; shift 2 ;;
    --skip-up) SKIP_UP=1; shift ;;
    --with-chat) WITH_CHAT=1; shift ;;
    -h|--help)
      cat <<EOF
Usage: $(basename "$0") --profile lossy|asym|bw [--status-url URL] [--skip-up] [--with-chat]

  lossy  N-HARD-LOSSY — netem both legs; N-HARD-FORCE under impairment
  asym   N-HARD-ASYM  — netem peer-a only; N-HARD-FORCE under impairment
  bw     N-HARD-BW    — tbf both legs; B-HARD-CALL (optional --with-chat)

Netem may retry the child smoke once (HARD_LAB flake policy).
EOF
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

[[ -n "${PROFILE}" ]] || pp_hard_die "missing --profile lossy|asym|bw"

case "${PROFILE}" in
  lossy) LABEL="N-HARD-LOSSY" ;;
  asym) LABEL="N-HARD-ASYM" ;;
  bw) LABEL="N-HARD-BW" ;;
  *) pp_hard_die "unknown --profile ${PROFILE} (lossy|asym|bw)" ;;
esac

pp_hard_ensure_up "${SKIP_UP}"
pp_hard_link_apply "${PROFILE}"

run_child() {
  case "${PROFILE}" in
    lossy|asym)
      bash "${ROOT}/scripts/test/pp_hard_force_smoke.sh" \
        --status-url "${PP_HARD_STATUS_URL}" --skip-up
      ;;
    bw)
      local chat_args=()
      if [[ "${WITH_CHAT}" -eq 1 ]]; then
        chat_args=(--with-chat)
      fi
      bash "${ROOT}/scripts/test/pp_hard_call_smoke.sh" \
        --status-url "${PP_HARD_STATUS_URL}" --skip-up "${chat_args[@]}"
      ;;
  esac
}

echo "=== ${LABEL} (link=${PROFILE}) ==="
pp_hard_run_with_netem_retry "${PROFILE}" run_child
pp_hard_assert_hop_alive
# Leave qdisc in place until next ensure_up/clear so operators can inspect; suite clears between profiles.
echo "${LABEL} smoke PASSED"
