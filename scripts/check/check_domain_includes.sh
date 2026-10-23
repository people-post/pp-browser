#!/usr/bin/env bash
# Guard domain-layer include edges. See docs/architecture/SRC_LAYOUT.md (Domain) and src/domain/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FAIL=0

# The domain modules, in their order: a module may include only the ones before it.
ORDER=(registration node call_media peers attachments)

check_absent() {
  local label="$1"
  local pattern="$2"
  local path="$3"
  if rg -q --glob '*.{h,hpp,c,cc,cpp,cxx,mm}' "$pattern" "$ROOT/$path" 2>/dev/null; then
    echo "FAIL: $label"
    rg --glob '*.{h,hpp,c,cc,cpp,cxx,mm}' "$pattern" "$ROOT/$path" || true
    FAIL=1
  fi
}

# Downward only: domain sits on lib / foundation / common (and the mesh, Amp).
check_absent "domain must not include feature/, gui/ or app/" \
  '#include "(feature|gui|app)/' src/domain
for lower in common foundation lib; do
  check_absent "$lower must not include domain/" '#include "domain/' "src/$lower"
done
# Rml-free: what a screen shows is decided in feature (presenters) and drawn in gui.
check_absent "domain must not include RmlUi" '#include [<"](RmlUi|ui/Core\.h|ui/base/)' src/domain

# Every folder under src/domain/ is in ORDER.
for dir in "$ROOT"/src/domain/*/; do
  name="$(basename "$dir")"
  found=0
  for m in "${ORDER[@]}"; do
    [[ "$m" == "$name" ]] && found=1
  done
  if [[ "$found" -eq 0 ]]; then
    echo "FAIL: src/domain/$name is not in ORDER (scripts/check/check_domain_includes.sh)"
    FAIL=1
  fi
done

# A module includes only the modules before it.
for i in "${!ORDER[@]}"; do
  module="${ORDER[$i]}"
  later=("${ORDER[@]:$((i + 1))}")
  if [[ ${#later[@]} -eq 0 ]]; then
    continue
  fi
  pattern="#include \"domain/($(IFS='|'; echo "${later[*]}"))/"
  check_absent "domain/$module must not include a module after it (${later[*]})" "$pattern" "src/domain/$module"
done

# Headless (pp-node links them): no UI / media / messaging peers.
check_absent "registration must not include lib/ui|media/" \
  '#include "lib/(ui|media)/' src/domain/registration
check_absent "node must not include lib/ui|media|messaging/" \
  '#include "lib/(ui|media|messaging)/' src/domain/node

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi

echo "OK: domain include edges"
