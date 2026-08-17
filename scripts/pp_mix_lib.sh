# Parallel child runner for interference mix smokes.
# Source from other scripts. Each child keeps its own pass/fail; the mix
# reports which names failed. See docs/ops/TEST_STRATEGY.md (B-MIX / N-MIX).
# shellcheck shell=bash

# pp_mix_run_parallel NAME CMD [NAME CMD ...]
# CMD is executed with bash -c. Logs go to a temp dir (printed).
pp_mix_run_parallel() {
  if [[ $(($# % 2)) -ne 0 || $# -lt 2 ]]; then
    echo "error: pp_mix_run_parallel expects NAME CMD pairs" >&2
    return 2
  fi
  local logdir
  logdir="$(mktemp -d /tmp/pp-mix.XXXXXX)"
  echo "mix logs: ${logdir}"

  local -a names=()
  local -a pids=()
  while [[ $# -gt 0 ]]; do
    local name="$1"
    local cmd="$2"
    shift 2
    names+=("${name}")
    bash -c "${cmd}" >"${logdir}/${name}.log" 2>&1 &
    pids+=("$!")
    echo "mix start child=${name} pid=${pids[-1]} log=${logdir}/${name}.log"
  done

  trap 'for _pid in "${pids[@]}"; do kill "${_pid}" 2>/dev/null || true; done' INT TERM

  local fail=0
  local -a failed=()
  local i rc
  for i in "${!names[@]}"; do
    rc=0
    wait "${pids[$i]}" || rc=$?
    if [[ "${rc}" -ne 0 ]]; then
      fail=1
      failed+=("${names[$i]}")
      echo "FAIL mix child=${names[$i]} rc=${rc} log=${logdir}/${names[$i]}.log"
      sed 's/^/  /' "${logdir}/${names[$i]}.log" || true
    else
      echo "ok  mix child=${names[$i]}"
    fi
  done

  trap - INT TERM
  if [[ "${fail}" -ne 0 ]]; then
    echo "error: mix failed children: ${failed[*]} (logs ${logdir})" >&2
    return 1
  fi
  echo "mix parallel PASSED children=${names[*]} logs=${logdir}"
  return 0
}
