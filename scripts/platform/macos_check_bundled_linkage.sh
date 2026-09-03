#!/usr/bin/env bash
# Fail if a macOS Mach-O still links Homebrew / Cellar / /usr/local dylibs.
# Usage: ./scripts/macos_check_bundled_linkage.sh path/to/PP.app [path/to/binary...]
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <PP.app|Mach-O> [more...]" >&2
  exit 2
fi

targets=()
for arg in "$@"; do
  if [[ -d "$arg" && "$arg" == *.app ]]; then
    exe="$(find "$arg/Contents/MacOS" -type f -perm +111 2>/dev/null | head -1 || true)"
    if [[ -z "$exe" ]]; then
      echo "error: no executable under ${arg}/Contents/MacOS" >&2
      exit 1
    fi
    targets+=("$exe")
  elif [[ -f "$arg" ]]; then
    targets+=("$arg")
  else
    echo "error: not a file or .app: ${arg}" >&2
    exit 1
  fi
done

bad=0
for bin in "${targets[@]}"; do
  echo "==> otool -L ${bin}"
  libs="$(otool -L "$bin" | awk 'NR>1 {print $1}')"
  printf '%s\n' "$libs"
  if printf '%s\n' "$libs" | grep -Eq '/opt/homebrew|/usr/local/(opt|Cellar)|/Cellar/'; then
    echo "error: ${bin} links non-vendored Homebrew/Cellar dylibs (not shippable under Developer ID)" >&2
    bad=1
  fi
done

exit "$bad"
