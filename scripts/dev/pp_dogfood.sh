#!/usr/bin/env bash
# Launch a local pp-browser build for dogfooding: core dumps on, and on a crash keep a copy of
# crash_pending.txt plus a symbolized backtrace next to it.
#
# Linux + macOS. The app itself writes {data_dir}/logs/pp-browser.log (see
# docs/ops/CONFIGURATION.md § Log file); this script adds crash capture around it and tees the raw
# console (non-logger output: RmlUi, pointer traces, libva…) to {data_dir}/logs/console.log.
#
# Examples:
#   ./scripts/dev/pp_dogfood.sh
#   ./scripts/dev/pp_dogfood.sh --build-dir build-ui-dev -- --debug
#   ./scripts/dev/pp_dogfood.sh --symbolize ~/.local/share/pp-browser/diagnostics/crash_pending.txt
#
set -uo pipefail

# Files this script writes (console tee, crash copies) carry PeerIds — owner-only like the app log.
APP_UMASK="$(umask)"
umask 077

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
DATA_DIR_OVERRIDE=""
SYMBOLIZE_ONLY=""
APP_ARGS=()

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- app args...]

  --build-dir DIR    Build tree holding src/app/pp-browser (default: build)
  --data-dir PATH    Override data root (default: platform data dir; -sandbox with --sandbox)
  --symbolize FILE   Only symbolize an existing crash dump against the build, then exit
  -h, --help         Show this help

Unknown options are passed to pp-browser. Log: {data_dir}/logs/pp-browser.log
EOF
}

detect_data_dir() {
  local product="pp-browser"
  local arg
  for arg in "${APP_ARGS[@]+"${APP_ARGS[@]}"}"; do
    [[ "$arg" == "--sandbox" ]] && product="pp-browser-sandbox"
  done
  case "$(uname -s)" in
    Darwin) echo "${HOME}/Library/Application Support/${product}/data" ;;
    *) echo "${XDG_DATA_HOME:-${HOME}/.local/share}/${product}" ;;
  esac
}

resolve_binary() {
  local bin="${BUILD_DIR}/src/app/pp-browser"
  if [[ -x "${bin}.app/Contents/MacOS/pp-browser" ]]; then
    echo "${bin}.app/Contents/MacOS/pp-browser"
  else
    echo "$bin"
  fi
}

git_head() {
  git -C "$1" rev-parse --short HEAD 2>/dev/null || echo "?"
}

# Load base from the dump's image_base=, else infer it (Linux) from the OnFatalSignal frame:
# the handler is small, so frame - symbol floors to the page-aligned base.
dump_image_base() {
  local dump="$1" bin="$2" base sym frame
  base="$(sed -n 's/^image_base=//p' "$dump" | head -1)"
  if [[ -n "$base" ]]; then
    echo "$base"
    return
  fi
  [[ "$(uname -s)" == "Darwin" ]] && return
  sym="$(nm -C "$bin" 2>/dev/null | awk '/OnFatalSignal\(int\)/ {print $1; exit}')"
  frame="$(grep -m2 -E '^0x[0-9a-fA-F]+$' "$dump" | tail -1)"
  [[ -z "$sym" || -z "$frame" ]] && return
  printf '0x%x\n' $(( ((frame - 16#$sym) / 4096) * 4096 ))
}

symbolize_dump() {
  local dump="$1" bin="$2" out="$3" base frame
  base="$(dump_image_base "$dump" "$bin")"
  {
    echo "binary=${bin}"
    echo "image_base=${base:-unknown}"
    echo "--- symbolized (main image; shared-lib frames shown raw) ---"
    if [[ -z "$base" ]]; then
      echo "(no image base — cannot symbolize)"
    else
      while read -r frame; do
        if (( frame < base )); then
          echo "${frame}  (outside main image)"
        elif [[ "$(uname -s)" == "Darwin" ]]; then
          atos -o "$bin" -l "$base" "$frame" 2>/dev/null
        else
          # Return addresses point after the call — step back one byte into the call site.
          addr2line -Cfip -e "$bin" "$(printf '0x%x' $(( frame - base - 1 )))" 2>/dev/null |
            sed -E 's#std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >#std::string#g'
        fi
      done < <(sed '/^--- breadcrumbs ---$/q' "$dump" | grep -E '^0x[0-9a-fA-F]+$')
    fi
    echo "--- raw dump ---"
    cat "$dump"
  } >"$out"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$(cd "$2" && pwd)"; shift 2 ;;
    --data-dir) DATA_DIR_OVERRIDE="$2"; shift 2 ;;
    --symbolize) SYMBOLIZE_ONLY="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; APP_ARGS+=("$@"); break ;;
    *) APP_ARGS+=("$1"); shift ;;
  esac
done

BIN="$(resolve_binary)"
if [[ ! -x "$BIN" ]]; then
  echo "error: no pp-browser binary at ${BIN} (build first or pass --build-dir)" >&2
  exit 2
fi

if [[ -n "$SYMBOLIZE_ONLY" ]]; then
  out="${SYMBOLIZE_ONLY%.txt}-symbolized.txt"
  symbolize_dump "$SYMBOLIZE_ONLY" "$BIN" "$out"
  echo "symbolized: ${out}"
  exit 0
fi

DATA_DIR="${DATA_DIR_OVERRIDE:-$(detect_data_dir)}"
DIAG_DIR="${DATA_DIR}/diagnostics"
PENDING="${DIAG_DIR}/crash_pending.txt"
mkdir -p "$DIAG_DIR"
MARKER="$(mktemp)"
trap 'rm -f "$MARKER"' EXIT

ulimit -c unlimited 2>/dev/null || true

echo "[dogfood] binary:   ${BIN}"
echo "[dogfood] revision: pp-browser $(git_head "$REPO_ROOT")$(
  [[ -d "${REPO_ROOT}/../pp-cpp-amp" ]] && echo ", pp-cpp-amp $(git_head "${REPO_ROOT}/../pp-cpp-amp")")"
echo "[dogfood] log:      ${DATA_DIR}/logs/pp-browser.log"
echo "[dogfood] console:  ${DATA_DIR}/logs/console.log"

mkdir -p "${DATA_DIR}/logs"
(umask "$APP_UMASK" && exec "$BIN" "${APP_ARGS[@]+"${APP_ARGS[@]}"}") 2>&1 | tee "${DATA_DIR}/logs/console.log"
status=${PIPESTATUS[0]}
echo "[dogfood] pp-browser exited status=${status}"

if [[ -f "$PENDING" && "$PENDING" -nt "$MARKER" ]]; then
  # The next launch may upload and clear crash_pending.txt — keep a copy per crash.
  stamp="$(date +%Y%m%d-%H%M%S)"
  saved="${DIAG_DIR}/crash-${stamp}.txt"
  cp "$PENDING" "$saved"
  # The launch log becomes pp-browser.1.log on the next start — keep it with the crash.
  cp "${DATA_DIR}/logs/pp-browser.log" "${DIAG_DIR}/crash-${stamp}.log" 2>/dev/null
  cp "${DATA_DIR}/logs/console.log" "${DIAG_DIR}/crash-${stamp}-console.log" 2>/dev/null
  symbolize_dump "$saved" "$BIN" "${DIAG_DIR}/crash-${stamp}-symbolized.txt"
  echo "[dogfood] CRASH captured:"
  echo "[dogfood]   dump:       ${saved}"
  echo "[dogfood]   symbolized: ${DIAG_DIR}/crash-${stamp}-symbolized.txt"
  echo "[dogfood]   log:        ${DIAG_DIR}/crash-${stamp}.log"
  echo "[dogfood]   console:    ${DIAG_DIR}/crash-${stamp}-console.log"
fi
exit "$status"
