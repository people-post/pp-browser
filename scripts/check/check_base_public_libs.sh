#!/usr/bin/env bash
# Guard domain peer CMake PUBLIC_LIBS edges (North Star).
# Mirrors scripts/check/check_base_includes.sh legacy allowlist for link edges.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
domain = {
    "pp_domain_people",
    "pp_domain_media",
    "pp_domain_net",
    "pp_domain_messaging",
    "pp_domain_mesh",
    "pp_domain_ai",
    "pp_domain_ui",
}
legacy = {
}

def peer_of(lib):
    """Sub-libraries (pp_domain_mesh_host, pp_domain_ai_mcp, ...) belong to their peer."""
    if lib in domain:
        return lib
    for peer in sorted(domain, key=len, reverse=True):
        if lib.startswith(peer + "_"):
            return peer
    return None

fail = 0
# Every pp_domain_* name in a peer's (non-test) CMake — helper PUBLIC_LIBS, variables, raw
# target_link_libraries — must belong to that peer. Sub-libraries count as their peer.
domain_root = root / "src" / "domain"
for peer_dir in sorted(p for p in domain_root.iterdir() if p.is_dir()):
    owner = f"pp_domain_{peer_dir.name}"
    if owner not in domain:
        continue
    for cmake in sorted(peer_dir.rglob("CMakeLists.txt")):
        if "tests" in cmake.relative_to(peer_dir).parts:
            continue
        for lib in sorted(set(re.findall(r"\b(pp_(?:base|domain)_[A-Za-z0-9_]+)", cmake.read_text()))):
            peer = peer_of(lib)
            if peer is None or peer == owner:
                continue
            edge = f"{owner}->{peer}"
            if edge not in legacy:
                print(f"FAIL: new domain peer link edge {edge} ({lib})")
                print(f"  {cmake.relative_to(root)}")
                print("  Wire via common ports / feature; do not add peer→peer links.")
                fail = 1
sys.exit(fail)
PY

echo "OK: base/domain PUBLIC_LIBS edges"

# Foundation CMake targets must keep pp_foundation_* names (no pp_base_* aliases).
if rg -n '\bpp_base_(error|i18n|runtime(_core)?|platform(_core)?|data|crypto)\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' "$ROOT" >/tmp/pp_foundation_rename.txt 2>/dev/null; then
  echo "FAIL: leftover transitional foundation lib names (use pp_foundation_*):"
  cat /tmp/pp_foundation_rename.txt
  exit 1
fi
echo "OK: foundation lib names"

# people must stay on pp_domain_people (no old target name aliases in CMake/code).
if rg -n '\bpp_base_people\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' --glob '!scripts/**' \
  "$ROOT" >/tmp/pp_people_rename.txt 2>/dev/null; then
  echo "FAIL: leftover pp_base_people (use pp_domain_people):"
  cat /tmp/pp_people_rename.txt
  exit 1
fi
echo "OK: people domain lib name"

# media must stay on pp_domain_media (no old target name aliases in CMake/code).
if rg -n '\bpp_base_media\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' --glob '!scripts/**' \
  "$ROOT" >/tmp/pp_media_rename.txt 2>/dev/null; then
  echo "FAIL: leftover pp_base_media (use pp_domain_media):"
  cat /tmp/pp_media_rename.txt
  exit 1
fi
echo "OK: media domain lib name"

# net must stay on pp_domain_net (no old target name aliases in CMake/code).
if rg -n '\bpp_base_net\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' --glob '!scripts/**' \
  "$ROOT" >/tmp/pp_net_rename.txt 2>/dev/null; then
  echo "FAIL: leftover pp_base_net (use pp_domain_net):"
  cat /tmp/pp_net_rename.txt
  exit 1
fi
echo "OK: net domain lib name"

# identity must stay on pp_foundation_identity (no pp_base_mesh_identity aliases).
if rg -n '\bpp_base_mesh_identity\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' --glob '!scripts/**' \
  "$ROOT" >/tmp/pp_identity_rename.txt 2>/dev/null; then
  echo "FAIL: leftover pp_base_mesh_identity (use pp_foundation_identity):"
  cat /tmp/pp_identity_rename.txt
  exit 1
fi
echo "OK: identity foundation lib name"

# ui host lives in foundation/platform; shell is pp_domain_ui (no pp_base_ui / pp_base_render).
if rg -n '\bpp_base_(ui|render)\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' --glob '!scripts/**' \
  "$ROOT" >/tmp/pp_ui_render_rename.txt 2>/dev/null; then
  echo "FAIL: leftover pp_base_ui / pp_base_render (use pp_domain_ui / foundation platform ui):"
  cat /tmp/pp_ui_render_rename.txt
  exit 1
fi
echo "OK: ui/render lib names"


# messaging must stay on pp_domain_messaging (no old target name aliases).
if rg -n '\bpp_base_messaging\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' --glob '!scripts/**' \
  "$ROOT" >/tmp/pp_messaging_rename.txt 2>/dev/null; then
  echo "FAIL: leftover pp_base_messaging (use pp_domain_messaging):"
  cat /tmp/pp_messaging_rename.txt
  exit 1
fi
echo "OK: messaging domain lib name"

# ai must stay on pp_domain_ai* (no old pp_base_ai* target aliases).
if rg -n '\bpp_base_ai(_conversation|_mcp)?\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' --glob '!scripts/**' \
  "$ROOT" >/tmp/pp_ai_rename.txt 2>/dev/null; then
  echo "FAIL: leftover pp_base_ai* (use pp_domain_ai*):"
  cat /tmp/pp_ai_rename.txt
  exit 1
fi
echo "OK: ai domain lib names"

# Product mesh target must stay pp_domain_mesh (no pp_base_mesh product target).
if rg -n '\bpp_base_mesh\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' --glob '!scripts/**' \
  --glob '!docs/**' --glob '!projects/**' --glob '!AGENTS.md' \
  "$ROOT" >/tmp/pp_mesh_rename.txt 2>/dev/null; then
  echo "FAIL: leftover pp_base_mesh (use pp_domain_mesh):"
  cat /tmp/pp_mesh_rename.txt
  exit 1
fi
echo "OK: mesh domain lib name"

# Amp FetchContent targets use pp_amp_* (no legacy pp_base_adp / pp_base_mesh_* aliases).
if rg -n '\bpp_base_(adp|mesh_session|mesh_channel|mesh_link)\b' \
  --glob '!build/**' --glob '!.git/**' --glob '!third_party/**' --glob '!scripts/**' \
  --glob '!docs/**' --glob '!projects/**' --glob '!AGENTS.md' \
  "$ROOT" >/tmp/pp_amp_alias_rename.txt 2>/dev/null; then
  echo "FAIL: leftover Amp pp_base_* aliases (use pp_amp_l1/l2/l3/link):"
  cat /tmp/pp_amp_alias_rename.txt
  exit 1
fi
echo "OK: Amp pp_amp_* target names"
