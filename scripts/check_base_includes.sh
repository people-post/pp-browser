#!/usr/bin/env bash
# Guard foundation/domain include edges (North Star).
# See docs/architecture/SRC_LAYOUT.md and src/base/README.md.
#
# Rules:
# 1) Hard bans (historical cycle points) — always fail.
# 2) Domain peers must not include each other unless the edge is on the
#    LEGACY_DOMAIN_EDGES allowlist (peel over time; do not add new edges).
# 3) base/mesh/identity/ is treated as foundation (PeerId) — people may include it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

check_absent() {
  local label="$1"
  local pattern="$2"
  local path="$3"
  if rg -q "$pattern" "$ROOT/$path" 2>/dev/null; then
    echo "FAIL: $label"
    rg -n "$pattern" "$ROOT/$path" || true
    FAIL=1
  fi
}

# --- Hard bans (keep forever) ---
check_absent "data must not include ai/" '#include "base/ai/' src/base/data
check_absent "crypto must not include messaging/ThreadTypes.h" \
  '#include "base/messaging/ThreadTypes.h"' src/base/crypto
check_absent "crypto must not include common/thread/ThreadTypes.h" \
  '#include "common/thread/ThreadTypes.h"' src/base/crypto
check_absent "messaging must not include ai/conversation/ConversationTypes.h" \
  '#include "base/ai/conversation/ConversationTypes.h"' src/base/messaging
check_absent "platform headers must not include ui/" \
  '#include "base/ui/' src/base/platform
check_absent "common must not include base/" \
  '#include "base/' src/common
check_absent "common must not include feature/" \
  '#include "feature/' src/common

# Legacy domain→domain edges still present. Remove entries as peels land.
# Format: from->to (module folder names under src/base/).
LEGACY_DOMAIN_EDGES=$(cat <<'EOF'
messaging->people
net->messaging
net->people
EOF
)

DOMAIN_MODULES=(people messaging net mesh media ai ui render)

is_legacy_edge() {
  local edge="$1"
  printf '%s\n' "$LEGACY_DOMAIN_EDGES" | grep -qx "$edge"
}

# Scan production sources (exclude tests/) for domain→domain includes.
while IFS= read -r -d '' file; do
  rel="${file#"$ROOT/"}"
  from=""
  for d in "${DOMAIN_MODULES[@]}"; do
    case "$rel" in
      src/base/"$d"/*) from="$d"; break ;;
    esac
  done
  [[ -n "$from" ]] || continue

  while IFS= read -r line; do
    dest=$(sed -n 's/.*#include "base\/\([^/]*\)\/.*/\1/p' <<<"$line")
    [[ -n "$dest" ]] || continue
    [[ "$dest" != "$from" ]] || continue

    # mesh/identity is foundation vocabulary for PeerId.
    if [[ "$dest" == "mesh" ]] && grep -q 'base/mesh/identity/' <<<"$line"; then
      continue
    fi

    # Only enforce among domain peers.
    is_domain_dest=0
    for d in "${DOMAIN_MODULES[@]}"; do
      if [[ "$dest" == "$d" ]]; then
        is_domain_dest=1
        break
      fi
    done
    [[ "$is_domain_dest" -eq 1 ]] || continue

    edge="$from->$dest"
    if ! is_legacy_edge "$edge"; then
      echo "FAIL: new domain peer edge $edge (not on legacy allowlist)"
      echo "  $rel: $line"
      echo "  Promote a contract to src/common/ or wire in feature/; do not add peer→peer includes."
      FAIL=1
    fi
  done < <(rg -n '#include "base/(people|messaging|net|mesh|media|ai|ui|render)/' "$file" || true)
done < <(find "$ROOT/src/base" \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' -o -name '*.mm' \) \
  ! -path '*/tests/*' -print0)

# Peeled paths must stay peeled (old locations / shims removed).
check_absent "mesh must not include people/RelayScope.h (use common/directory/RelayScope.h)" \
  '#include "base/people/RelayScope.h"' src/base/mesh
check_absent "mesh must not include people/ContactTypes.h (use common/directory/DirectoryTypes.h)" \
  '#include "base/people/ContactTypes.h"' src/base/mesh
check_absent "mesh production must not include base/net/ (use common/directory/IDirectoryClient.h)" \
  '#include "base/net/' src/base/mesh/discovery
check_absent "mesh must not include base/media/ (use common/media/CallMediaHealth.h)" \
  '#include "base/media/' src/base/mesh
check_absent "ai must not include ui/WorkingSetTypes.h (use common/ui/WorkingSetTypes.h)" \
  '#include "base/ui/WorkingSetTypes.h"' src/base/ai
check_absent "ai must not include messaging/ChatActionTypes.h (use common/chat/)" \
  '#include "base/messaging/ChatActionTypes.h"' src/base/ai
check_absent "ai must not include messaging/ThreadMemoryTypes.h (use common/thread/)" \
  '#include "base/messaging/ThreadMemoryTypes.h"' src/base/ai
check_absent "ai must not include messaging/ThreadTypes.h (use common/thread/)" \
  '#include "base/messaging/ThreadTypes.h"' src/base/ai
check_absent "ai must not include messaging/IThreadStore.h (use common/thread/)" \
  '#include "base/messaging/IThreadStore.h"' src/base/ai
check_absent "ai must not include messaging/PeopleDiscoveryBlocks.h (use common/chat/)" \
  '#include "base/messaging/PeopleDiscoveryBlocks.h"' src/base/ai
check_absent "ai must not include base/net/ (use common/net/HttpTransport + feature wiring)" \
  '#include "base/net/' src/base/ai
check_absent "messaging must not include people/MeshHopPolicy.h (use common/directory/MeshHopTypes.h)" \
  '#include "base/people/MeshHopPolicy.h"' src/base/messaging
check_absent "net must not include messaging/RelayStreamKey.h (use common/chat/)" \
  '#include "base/messaging/RelayStreamKey.h"' src/base/net
check_absent "net must not include messaging/ThreadTypes.h (use common/thread/)" \
  '#include "base/messaging/ThreadTypes.h"' src/base/net
check_absent "net must not include messaging/ChatPayloadTypes.h (use common/chat/)" \
  '#include "base/messaging/ChatPayloadTypes.h"' src/base/net
check_absent "net must not include messaging/IThreadStore.h (use common/thread/)" \
  '#include "base/messaging/IThreadStore.h"' src/base/net
check_absent "messaging must not include base/net/AttachmentClientUtil.h (limit in common/chat/MessagingLimits.h)" \
  '#include "base/net/AttachmentClientUtil.h"' src/base/messaging
check_absent "messaging must not include feature/ (domain may not include feature)" \
  '#include "feature/' src/base/messaging

# Removed shim paths must stay deleted.
for shim in \
  src/common/ThreadTypes.h \
  src/common/IThreadStore.h \
  src/common/CallMediaHealth.h \
  src/base/messaging/ThreadTypes.h \
  src/base/messaging/IThreadStore.h \
  src/base/data/ContextBudget.h \
  src/base/people/RelayScope.h \
  src/base/net/CurlSsl.h \
  src/base/media/CallMediaHealth.h \
  src/base/ui/WorkingSetTypes.h \
  src/base/messaging/PeopleDiscoveryBlocks.h
do
  if [[ -e "$ROOT/$shim" ]]; then
    echo "FAIL: shim path still exists: $shim"
    FAIL=1
  fi
done

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi

echo "OK: base/domain include edges"
