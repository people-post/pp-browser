#!/usr/bin/env bash
# Guard foundation/domain include edges (North Star).
# See docs/architecture/SRC_LAYOUT.md and src/foundation/README.md / src/domain/README.md.
#
# Rules:
# 1) Hard bans (historical cycle points) — always fail.
# 2) Domain peers must not include each other unless the edge is on the
#    LEGACY_DOMAIN_EDGES allowlist (peel over time; do not add new edges).
# 3) PeerId lives in foundation/identity/ (not under mesh).
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

# Same as check_absent but ignore tests/ (production domain sources only).
check_absent_prod() {
  local label="$1"
  local pattern="$2"
  local path="$3"
  if rg -q "$pattern" "$ROOT/$path" -g '!**/tests/**' 2>/dev/null; then
    echo "FAIL: $label"
    rg -n "$pattern" "$ROOT/$path" -g '!**/tests/**' || true
    FAIL=1
  fi
}

# --- Hard bans (keep forever) ---
check_absent "data must not include base/ai/" '#include "base/ai/' src/foundation/data
check_absent "data must not include domain/ai/" '#include "domain/ai/' src/foundation/data
check_absent "crypto must not include base/messaging/ThreadTypes.h" \
  '#include "base/messaging/ThreadTypes.h"' src/foundation/crypto
check_absent "crypto must not include domain/messaging/ThreadTypes.h" \
  '#include "domain/messaging/ThreadTypes.h"' src/foundation/crypto
check_absent "crypto must not include common/thread/ThreadTypes.h" \
  '#include "common/thread/ThreadTypes.h"' src/foundation/crypto
check_absent "messaging must not include ai/conversation/ConversationTypes.h" \
  '#include "domain/ai/conversation/ConversationTypes.h"' src/domain/messaging
check_absent "platform headers must not include ui/" \
  '#include "domain/ui/' src/foundation/platform
check_absent "common must not include base/" \
  '#include "base/' src/common
check_absent "common must not include feature/" \
  '#include "feature/' src/common

# Legacy domain→domain edges still present. Remove entries as peels land.
# Format: from->to (module folder names under src/domain/).
# (empty — all historical domain peer edges cleared)
LEGACY_DOMAIN_EDGES=$(cat <<'EOF'
EOF
)

DOMAIN_MODULES=(people messaging net mesh media ai ui)

is_legacy_edge() {
  local edge="$1"
  printf '%s\n' "$LEGACY_DOMAIN_EDGES" | grep -qx "$edge"
}

# Scan production sources under src/base and src/domain for domain→domain includes.
scan_domain_peer_includes() {
  local root_dir="$1"
  while IFS= read -r -d '' file; do
    rel="${file#"$ROOT/"}"
    from=""
    for d in "${DOMAIN_MODULES[@]}"; do
      case "$rel" in
        src/base/"$d"/*|src/domain/"$d"/*) from="$d"; break ;;
      esac
    done
    [[ -n "$from" ]] || continue

    while IFS= read -r line; do
      dest=$(sed -n 's/.*#include "\(base\|domain\)\/\([^/]*\)\/.*/\2/p' <<<"$line")
      [[ -n "$dest" ]] || continue
      [[ "$dest" != "$from" ]] || continue

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
    done < <(rg -n '#include "(base|domain)/(people|messaging|net|mesh|media|ai|ui)/' "$file" || true)
  done < <(find "$root_dir" \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' -o -name '*.mm' \) \
    ! -path '*/tests/*' -print0)
}

# Domain peers live under src/domain/ only (src/base/ retired).
if [[ -d "$ROOT/src/domain" ]]; then
  scan_domain_peer_includes "$ROOT/src/domain"
fi
if [[ -d "$ROOT/src/base" ]]; then
  echo "FAIL: src/base/ must remain deleted (see src/CMakeLists.txt pp_base aggregate)"
  FAIL=1
fi

check_absent "common must not include domain/" \
  '#include "domain/' src/common

check_absent "must not include base/mesh/identity/ (moved to foundation/identity/)" \
  '#include "domain/mesh/identity/' src
check_absent "common must not include foundation/" \
  '#include "foundation/' src/common

# Peeled paths must stay peeled (old locations / shims removed).
check_absent "mesh must not include people/RelayScope.h (use common/directory/RelayScope.h)" \
  '#include "domain/people/RelayScope.h"' src/domain/mesh
check_absent "mesh must not include people/ContactTypes.h (use common/directory/DirectoryTypes.h)" \
  '#include "domain/people/ContactTypes.h"' src/domain/mesh
check_absent "mesh production must not include base/net/ (use common/directory/IDirectoryClient.h)" \
  '#include "domain/net/' src/domain/mesh/discovery
check_absent "mesh must not include domain/media/ (use common/media/CallMediaHealth.h)" \
  '#include "domain/media/' src/domain/mesh
check_absent "ai must not include ui/WorkingSetTypes.h (use common/ui/WorkingSetTypes.h)" \
  '#include "domain/ui/WorkingSetTypes.h"' src/domain/ai
check_absent "ai must not include base/messaging/ChatActionTypes.h (use common/chat/)" \
  '#include "base/messaging/ChatActionTypes.h"' src/domain/ai
check_absent "ai must not include messaging/ChatActionTypes.h (use common/chat/)" \
  '#include "domain/messaging/ChatActionTypes.h"' src/domain/ai
check_absent "ai must not include base/messaging/ThreadMemoryTypes.h (use common/thread/)" \
  '#include "base/messaging/ThreadMemoryTypes.h"' src/domain/ai
check_absent "ai must not include messaging/ThreadMemoryTypes.h (use common/thread/)" \
  '#include "domain/messaging/ThreadMemoryTypes.h"' src/domain/ai
check_absent "ai must not include base/messaging/ThreadTypes.h (use common/thread/)" \
  '#include "base/messaging/ThreadTypes.h"' src/domain/ai
check_absent "ai must not include messaging/ThreadTypes.h (use common/thread/)" \
  '#include "domain/messaging/ThreadTypes.h"' src/domain/ai
check_absent "ai must not include base/messaging/IThreadStore.h (use common/thread/)" \
  '#include "base/messaging/IThreadStore.h"' src/domain/ai
check_absent "ai must not include messaging/IThreadStore.h (use common/thread/)" \
  '#include "domain/messaging/IThreadStore.h"' src/domain/ai
check_absent "ai must not include base/messaging/PeopleDiscoveryBlocks.h (use common/chat/)" \
  '#include "base/messaging/PeopleDiscoveryBlocks.h"' src/domain/ai
check_absent "ai must not include messaging/PeopleDiscoveryBlocks.h (use common/chat/)" \
  '#include "domain/messaging/PeopleDiscoveryBlocks.h"' src/domain/ai
check_absent "ai must not include base/net/ (use common/net/HttpTransport + feature wiring)" \
  '#include "domain/net/' src/domain/ai
check_absent "messaging must not include people/MeshHopPolicy.h (use common/directory/MeshHopTypes.h)" \
  '#include "domain/people/MeshHopPolicy.h"' src/domain/messaging
check_absent "net must not include base/messaging/RelayStreamKey.h (use common/chat/)" \
  '#include "base/messaging/RelayStreamKey.h"' src/domain/net
check_absent "net must not include messaging/RelayStreamKey.h (use common/chat/)" \
  '#include "domain/messaging/RelayStreamKey.h"' src/domain/net
check_absent "net must not include base/messaging/ThreadTypes.h (use common/thread/)" \
  '#include "base/messaging/ThreadTypes.h"' src/domain/net
check_absent "net must not include messaging/ThreadTypes.h (use common/thread/)" \
  '#include "domain/messaging/ThreadTypes.h"' src/domain/net
check_absent "net must not include base/messaging/ChatPayloadTypes.h (use common/chat/)" \
  '#include "base/messaging/ChatPayloadTypes.h"' src/domain/net
check_absent "net must not include messaging/ChatPayloadTypes.h (use common/chat/)" \
  '#include "domain/messaging/ChatPayloadTypes.h"' src/domain/net
check_absent "net must not include base/messaging/IThreadStore.h (use common/thread/)" \
  '#include "base/messaging/IThreadStore.h"' src/domain/net
check_absent "net must not include messaging/IThreadStore.h (use common/thread/)" \
  '#include "domain/messaging/IThreadStore.h"' src/domain/net
check_absent "net must not include base/messaging/MessagingJson.h (use common/chat/)" \
  '#include "base/messaging/MessagingJson.h"' src/domain/net
check_absent "net must not include messaging/MessagingJson.h (use common/chat/)" \
  '#include "domain/messaging/MessagingJson.h"' src/domain/net
check_absent "net must not include base/messaging/EnvelopeSigner.h (inject BuildSignBytes from messaging tests)" \
  '#include "base/messaging/EnvelopeSigner.h"' src/domain/net
check_absent "net must not include messaging/EnvelopeSigner.h (inject BuildSignBytes from messaging tests)" \
  '#include "domain/messaging/EnvelopeSigner.h"' src/domain/net
check_absent "net must not include base/messaging/RelayWirePayload.h (use common/chat or messaging tests)" \
  '#include "base/messaging/RelayWirePayload.h"' src/domain/net
check_absent "net must not include messaging/RelayWirePayload.h (use common/chat or messaging tests)" \
  '#include "domain/messaging/RelayWirePayload.h"' src/domain/net
check_absent_prod "messaging must not include domain/people/ (wire via common/feature)" \
  '#include "domain/people/' src/domain/messaging
check_absent_prod "net must not include domain/people/ (use common/directory + feature wiring)" \
  '#include "domain/people/' src/domain/net
check_absent "must not include base/people/ (moved to domain/people/)" \
  '#include "base/people/' src
check_absent "must not include base/media/ (moved to domain/media/)" \
  '#include "base/media/' src
check_absent "must not include base/net/ (moved to domain/net/)" \
  '#include "base/net/' src
check_absent "must not include base/messaging/ (moved to domain/messaging/)" \
  '#include "base/messaging/' src
check_absent "must not include base/ai/ (moved to domain/ai/)" \
  '#include "base/ai/' src
check_absent "must not include base/mesh/ (moved to domain/mesh/; identity → foundation/identity/)" \
  '#include "base/mesh/' src
check_absent "must not include base/ui/ (moved to domain/ui/)" \
  '#include "base/ui/' src
check_absent "must not include base/render/ (moved to foundation/platform/ui/)" \
  '#include "base/render/' src
check_absent "messaging must not include base/net/AttachmentClientUtil.h (limit in common/chat/MessagingLimits.h)" \
  '#include "base/net/AttachmentClientUtil.h"' src/domain/messaging
check_absent "must not include ChatBlobRequestUtil at base path (use feature/messaging/)" \
  '#include "base/messaging/ChatBlobRequestUtil.h"' src
check_absent "domain must not include ChatBlobRequestUtil at messaging path (use feature/messaging/)" \
  '#include "domain/messaging/ChatBlobRequestUtil.h"' src
check_absent "must not include base/net/ProfileIconClientUtil.h (use feature/messaging/)" \
  '#include "base/net/ProfileIconClientUtil.h"' src
check_absent "must not include base/net/RegistrationClientUtil.h (use feature/messaging/)" \
  '#include "base/net/RegistrationClientUtil.h"' src
check_absent "messaging must not include feature/ (domain may not include feature)" \
  '#include "feature/' src/domain/messaging

# Removed shim paths must stay deleted.
for shim in \
  src/base \
  src/common/ThreadTypes.h \
  src/common/IThreadStore.h \
  src/common/CallMediaHealth.h \
  src/base/messaging \
  src/base/ai \
  src/domain/messaging/ThreadTypes.h \
  src/domain/messaging/IThreadStore.h \
  src/domain/messaging/MessagingJson.h \
  src/domain/messaging/ChatBlobRequestUtil.h \
  src/base/data \
  src/base/crypto \
  src/base/error \
  src/base/i18n \
  src/domain/messaging/PeerBriefRoute.h \
  src/domain/messaging/ChatHistoryResponder.h \
  src/domain/mesh/host/MeshChannelLimits.h \
  src/domain/mesh/identity \
  src/base/net/ProfileIconClientUtil.h \
  src/base/net/ProfileIconFetchUtil.h \
  src/base/net/RegistrationClientUtil.h \
  src/base/data/ContextBudget.h \
  src/base/people \
  src/base/people/RelayScope.h \
  src/base/net/CurlSsl.h \
  src/base/media \
  src/base/net \
  src/base/media/CallMediaHealth.h \
  src/base/ui \
  src/base/render \
  src/base/ui/WorkingSetTypes.h \
  src/domain/messaging/PeopleDiscoveryBlocks.h \
  src/base/runtime \
  src/base/platform
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
