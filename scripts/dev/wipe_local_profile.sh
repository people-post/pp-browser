#!/usr/bin/env bash
# Wipe local pp-browser profile data after Brief PQ / Account ID hard cut.
#
# Linux + macOS. Windows: scripts/dev/wipe_local_profile.ps1
# Android / iOS: clear app storage by hand (Settings or adb pm clear).
#
# Default data roots (see docs/contracts/DATA_LAYOUT.md):
#   Linux:  $XDG_DATA_HOME/pp-browser or ~/.local/share/pp-browser
#   macOS:  ~/Library/Application Support/pp-browser/data
#
# Examples:
#   ./scripts/dev/wipe_local_profile.sh --dry-run
#   ./scripts/dev/wipe_local_profile.sh --yes
#   ./scripts/dev/wipe_local_profile.sh --yes --profile default
#   ./scripts/dev/wipe_local_profile.sh --yes --data-dir /path/to/data
#
set -euo pipefail

DRY_RUN=1
YES=0
PROFILE=""
DATA_DIR_OVERRIDE=""
WIPE_PROFILES_JSON=1

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

  --yes              Perform wipe (default is dry-run)
  --dry-run          List paths only (default)
  --profile NAME     Wipe only profiles/NAME (default: all profiles/)
  --data-dir PATH    Override data root
  --keep-registry    Keep profiles.json when wiping all profiles
  -h, --help         Show this help

Quit pp-browser before wiping. After wipe: launch → PIN → Register on network.
EOF
}

detect_data_dir() {
  local uname_s
  uname_s="$(uname -s)"
  case "$uname_s" in
    Darwin)
      echo "${HOME}/Library/Application Support/pp-browser/data"
      ;;
    Linux|*)
      if [[ -n "${XDG_DATA_HOME:-}" ]]; then
        echo "${XDG_DATA_HOME}/pp-browser"
      else
        echo "${HOME}/.local/share/pp-browser"
      fi
      ;;
  esac
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --yes) YES=1; DRY_RUN=0; shift ;;
    --dry-run) DRY_RUN=1; YES=0; shift ;;
    --profile) PROFILE="$2"; shift 2 ;;
    --data-dir) DATA_DIR_OVERRIDE="$2"; shift 2 ;;
    --keep-registry) WIPE_PROFILES_JSON=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

DATA_DIR="${DATA_DIR_OVERRIDE:-$(detect_data_dir)}"
PROFILES_DIR="${DATA_DIR}/profiles"
PROFILES_JSON="${DATA_DIR}/profiles.json"

echo "platform: $(uname -s)"
echo "data_dir: ${DATA_DIR}"

if [[ ! -d "$DATA_DIR" ]]; then
  echo "nothing to wipe — data dir missing"
  exit 0
fi

TARGETS=()
if [[ -n "$PROFILE" ]]; then
  TARGETS+=("${PROFILES_DIR}/${PROFILE}")
else
  if [[ -d "$PROFILES_DIR" ]]; then
    TARGETS+=("$PROFILES_DIR")
  fi
  if [[ "$WIPE_PROFILES_JSON" -eq 1 && -f "$PROFILES_JSON" ]]; then
    TARGETS+=("$PROFILES_JSON")
  fi
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  echo "nothing to wipe — no profiles under ${PROFILES_DIR}"
  exit 0
fi

echo "targets:"
for t in "${TARGETS[@]}"; do
  if [[ -e "$t" ]]; then
    echo "  $t"
  else
    echo "  $t (missing)"
  fi
done

if [[ "$DRY_RUN" -eq 1 || "$YES" -ne 1 ]]; then
  echo "dry-run complete — re-run with --yes to delete"
  exit 0
fi

for t in "${TARGETS[@]}"; do
  if [[ -e "$t" ]]; then
    rm -rf "$t"
    echo "removed $t"
  fi
done

echo "wipe complete — restart pp-browser and re-register"
