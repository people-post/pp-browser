#!/usr/bin/env bash
# Guard foundation/lib include edges (North Star).
# See docs/architecture/SRC_LAYOUT.md and src/foundation/README.md / src/lib/README.md.
#
# Rules:
# 1) Hard bans (historical cycle points) — always fail.
# 2) Lib peers must not include each other unless the edge is on the
#    LEGACY_DOMAIN_EDGES allowlist (peel over time; do not add new edges).
# 3) PeerId is derived by pp-cpp-crypto (crypto/PeerId.h), not under mesh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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

# Same as check_absent but ignore tests/ (production lib sources only).
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
check_absent "data must not include lib/ai/" '#include "lib/ai/' src/foundation/data
check_absent "crypto must not include base/messaging/ThreadTypes.h" \
  '#include "base/messaging/ThreadTypes.h"' src/foundation/crypto
check_absent "crypto must not include lib/messaging/ThreadTypes.h" \
  '#include "lib/messaging/ThreadTypes.h"' src/foundation/crypto
check_absent "crypto must not include common/thread/ThreadTypes.h" \
  '#include "common/thread/ThreadTypes.h"' src/foundation/crypto
check_absent "messaging must not include ai/conversation/ConversationTypes.h" \
  '#include "lib/ai/conversation/ConversationTypes.h"' src/lib/messaging
check_absent "platform headers must not include ui/" \
  '#include "lib/ui/' src/foundation/platform
check_absent "common must not include base/" \
  '#include "base/' src/common
check_absent "common must not include feature/" \
  '#include "feature/' src/common

# Legacy lib→lib edges still present. Remove entries as peels land.
# Format: from->to (module folder names under src/lib/).
# (empty — all historical lib peer edges cleared)
LEGACY_DOMAIN_EDGES=$(cat <<'EOF'
EOF
)

DOMAIN_MODULES=(people messaging calls broadcast pricing content net mesh media ai ui)

is_legacy_edge() {
  local edge="$1"
  printf '%s\n' "$LEGACY_DOMAIN_EDGES" | grep -qx "$edge"
}

# Scan production sources under src/lib for lib→lib includes.
scan_lib_peer_includes() {
  local root_dir="$1"
  while IFS= read -r -d '' file; do
    rel="${file#"$ROOT/"}"
    from=""
    for d in "${DOMAIN_MODULES[@]}"; do
      case "$rel" in
        src/lib/"$d"/*) from="$d"; break ;;
      esac
    done
    [[ -n "$from" ]] || continue

    while IFS= read -r line; do
      dest=$(sed -n 's/.*#include "\(base\|lib\)\/\([^/]*\)\/.*/\2/p' <<<"$line")
      [[ -n "$dest" ]] || continue
      [[ "$dest" != "$from" ]] || continue

      is_lib_dest=0
      for d in "${DOMAIN_MODULES[@]}"; do
        if [[ "$dest" == "$d" ]]; then
          is_lib_dest=1
          break
        fi
      done
      [[ "$is_lib_dest" -eq 1 ]] || continue

      edge="$from->$dest"
      if ! is_legacy_edge "$edge"; then
        echo "FAIL: new lib peer edge $edge (not on legacy allowlist)"
        echo "  $rel: $line"
        echo "  Promote a contract to src/common/ or wire in feature/; do not add peer→peer includes."
        FAIL=1
      fi
    done < <(rg -n '#include "(base|lib)/(people|messaging|calls|broadcast|pricing|content|net|mesh|media|ai|ui)/' "$file" || true)
  done < <(find "$root_dir" \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' -o -name '*.mm' \) \
    ! -path '*/tests/*' -print0)
}

# Lib peers live under src/lib/.
if [[ -d "$ROOT/src/lib" ]]; then
  scan_lib_peer_includes "$ROOT/src/lib"
fi

check_absent "common must not include lib/" \
  '#include "lib/' src/common

check_absent "must not include lib/mesh/ (moved to pp-cpp-mesh: mesh/…)" \
  '#include "lib/mesh/' src

# The app's lower layers may embed the mesh vocabulary (pp_mesh_types: pp-cpp-common only), never the
# mesh itself, which sits on Amp: that is feature/ and app/'s to compose.
lower_mesh=$(rg -n -g '*.h' -g '*.cpp' -g '*.mm' '#include "mesh/' \
  "$ROOT/src/common" "$ROOT/src/foundation" "$ROOT/src/lib" 2>/dev/null \
  | grep -v '#include "mesh/types/' || true)
if [[ -n "$lower_mesh" ]]; then
  echo "FAIL: common/ foundation/ lib/ may include mesh/types/ only (pp-cpp-mesh's vocabulary):"
  echo "$lower_mesh"
  FAIL=1
fi
check_absent "common must not include foundation/" \
  '#include "foundation/' src/common

# Peeled paths must stay peeled (old locations / shims removed).
check_absent "ai must not include ui/WorkingSetTypes.h (use common/ui/WorkingSetTypes.h)" \
  '#include "lib/ui/WorkingSetTypes.h"' src/lib/ai
check_absent "ai must not include base/messaging/ChatActionTypes.h (use common/chat/)" \
  '#include "base/messaging/ChatActionTypes.h"' src/lib/ai
check_absent "ai must not include messaging/ChatActionTypes.h (use common/chat/)" \
  '#include "lib/messaging/ChatActionTypes.h"' src/lib/ai
check_absent "ai must not include base/messaging/ThreadMemoryTypes.h (use common/thread/)" \
  '#include "base/messaging/ThreadMemoryTypes.h"' src/lib/ai
check_absent "ai must not include messaging/ThreadMemoryTypes.h (use common/thread/)" \
  '#include "lib/messaging/ThreadMemoryTypes.h"' src/lib/ai
check_absent "ai must not include base/messaging/ThreadTypes.h (use common/thread/)" \
  '#include "base/messaging/ThreadTypes.h"' src/lib/ai
check_absent "ai must not include messaging/ThreadTypes.h (use common/thread/)" \
  '#include "lib/messaging/ThreadTypes.h"' src/lib/ai
check_absent "ai must not include base/messaging/IThreadStore.h (use common/thread/)" \
  '#include "base/messaging/IThreadStore.h"' src/lib/ai
check_absent "ai must not include messaging/IThreadStore.h (use common/thread/)" \
  '#include "lib/messaging/IThreadStore.h"' src/lib/ai
check_absent "ai must not include base/messaging/PeopleDiscoveryBlocks.h (use common/chat/)" \
  '#include "base/messaging/PeopleDiscoveryBlocks.h"' src/lib/ai
check_absent "ai must not include messaging/PeopleDiscoveryBlocks.h (use common/chat/)" \
  '#include "lib/messaging/PeopleDiscoveryBlocks.h"' src/lib/ai
check_absent "ai must not include base/net/ (use common/net/HttpTransport + feature wiring)" \
  '#include "lib/net/' src/lib/ai
check_absent "messaging must not include people/MeshHopPolicy.h (use mesh/types/MeshHopTypes.h)" \
  '#include "lib/people/MeshHopPolicy.h"' src/lib/messaging
check_absent "net must not include base/messaging/RelayStreamKey.h (use common/chat/)" \
  '#include "base/messaging/RelayStreamKey.h"' src/lib/net
check_absent "net must not include messaging/RelayStreamKey.h (use common/chat/)" \
  '#include "lib/messaging/RelayStreamKey.h"' src/lib/net
check_absent "net must not include base/messaging/ThreadTypes.h (use common/thread/)" \
  '#include "base/messaging/ThreadTypes.h"' src/lib/net
check_absent "net must not include messaging/ThreadTypes.h (use common/thread/)" \
  '#include "lib/messaging/ThreadTypes.h"' src/lib/net
check_absent "net must not include base/messaging/ChatPayloadTypes.h (use common/chat/)" \
  '#include "base/messaging/ChatPayloadTypes.h"' src/lib/net
check_absent "net must not include messaging/ChatPayloadTypes.h (use common/chat/)" \
  '#include "lib/messaging/ChatPayloadTypes.h"' src/lib/net
check_absent "net must not include base/messaging/IThreadStore.h (use common/thread/)" \
  '#include "base/messaging/IThreadStore.h"' src/lib/net
check_absent "net must not include messaging/IThreadStore.h (use common/thread/)" \
  '#include "lib/messaging/IThreadStore.h"' src/lib/net
check_absent "net must not include base/messaging/MessagingJson.h (use common/chat/)" \
  '#include "base/messaging/MessagingJson.h"' src/lib/net
check_absent "net must not include messaging/MessagingJson.h (use common/chat/)" \
  '#include "lib/messaging/MessagingJson.h"' src/lib/net
check_absent "net must not include base/messaging/EnvelopeSigner.h (inject BuildSignBytes from messaging tests)" \
  '#include "base/messaging/EnvelopeSigner.h"' src/lib/net
check_absent "net must not include messaging/EnvelopeSigner.h (inject BuildSignBytes from messaging tests)" \
  '#include "lib/messaging/EnvelopeSigner.h"' src/lib/net
check_absent "net must not include base/messaging/RelayWirePayload.h (use common/chat or messaging tests)" \
  '#include "base/messaging/RelayWirePayload.h"' src/lib/net
check_absent "net must not include messaging/RelayWirePayload.h (use common/chat or messaging tests)" \
  '#include "lib/messaging/RelayWirePayload.h"' src/lib/net
check_absent_prod "messaging must not include lib/people/ (wire via common/feature)" \
  '#include "lib/people/' src/lib/messaging
check_absent_prod "net must not include lib/people/ (use common/directory + feature wiring)" \
  '#include "lib/people/' src/lib/net
check_absent "must not include base/people/ (moved to lib/people/)" \
  '#include "base/people/' src
check_absent "must not include base/media/ (moved to lib/media/)" \
  '#include "base/media/' src
check_absent "must not include base/net/ (moved to lib/net/)" \
  '#include "base/net/' src
check_absent "must not include base/messaging/ (moved to lib/messaging/)" \
  '#include "base/messaging/' src
check_absent "must not include base/ai/ (moved to lib/ai/)" \
  '#include "base/ai/' src
check_absent "must not include base/mesh/ (moved to pp-cpp-mesh; identity → foundation/identity/)" \
  '#include "base/mesh/' src
check_absent "must not include base/ui/ (moved to lib/ui/)" \
  '#include "base/ui/' src
check_absent "must not include base/render/ (moved to foundation/platform/ui/)" \
  '#include "base/render/' src
check_absent "messaging must not include base/net/AttachmentClientUtil.h (limit in common/chat/MessagingLimits.h)" \
  '#include "base/net/AttachmentClientUtil.h"' src/lib/messaging
check_absent "must not include ChatBlobRequestUtil at base path (use domain/registration/)" \
  '#include "base/messaging/ChatBlobRequestUtil.h"' src
check_absent "lib must not include ChatBlobRequestUtil at messaging path (use domain/registration/)" \
  '#include "lib/messaging/ChatBlobRequestUtil.h"' src
check_absent "must not include base/net/ProfileIconClientUtil.h (use domain/registration/)" \
  '#include "base/net/ProfileIconClientUtil.h"' src
check_absent "must not include base/net/RegistrationClientUtil.h (use domain/registration/)" \
  '#include "base/net/RegistrationClientUtil.h"' src
check_absent "messaging must not include feature/ (lib may not include feature)" \
  '#include "feature/' src/lib/messaging

# Peeled / relocated paths must stay deleted (include-path bans above catch code; this
# catches leftover files under common/lib only — not the retired src/base/ tree).
for shim in \
  src/common/ThreadTypes.h \
  src/common/IThreadStore.h \
  src/common/CallMediaHealth.h \
  src/lib/messaging/ThreadTypes.h \
  src/lib/messaging/IThreadStore.h \
  src/lib/messaging/MessagingJson.h \
  src/lib/messaging/ChatBlobRequestUtil.h \
  src/lib/messaging/PeerBriefRoute.h \
  src/lib/mesh \
  src/common/directory/DirectoryTypes.h \
  src/common/directory/IDirectoryClient.h \
  src/common/directory/MeshHopDial.h \
  src/common/directory/MeshHopTypes.h \
  src/common/directory/RelayScope.h \
  src/common/media/MediaChannel.h \
  src/common/net/PublicAddress.h \
  src/common/privacy/AddressDisclosure.h \
  src/foundation/data/MeshConfigTypes.h \
  src/lib/messaging/PeopleDiscoveryBlocks.h
do
  if [[ -e "$ROOT/$shim" ]]; then
    echo "FAIL: shim path still exists: $shim"
    FAIL=1
  fi
done

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi

echo "OK: foundation/lib include edges"
