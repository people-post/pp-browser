#!/usr/bin/env bash
# Guard domain peer CMake PUBLIC_LIBS edges (North Star).
# Mirrors scripts/check_base_includes.sh legacy allowlist for link edges.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
domain = {
    "pp_domain_people",
    "pp_domain_media",
    "pp_domain_net",
    "pp_base_messaging",
        "pp_base_mesh",
    "pp_base_ai",
    "pp_base_ai_conversation",
    "pp_base_ai_mcp",
    "pp_base_ui",
    "pp_base_render",
}
legacy = {
}

fail = 0
cmake_roots = [root / "src" / "base", root / "src" / "domain"]
lib_helpers = (
    r"pp_browser_add_(?:base|domain)_library\(\s*(pp_(?:base|domain)_[A-Za-z0-9_]+)\s*(.*?)^\s*\)"
)

for cmake_root in cmake_roots:
    if not cmake_root.is_dir():
        continue
    for cmake in sorted(cmake_root.rglob("CMakeLists.txt")):
        text = cmake.read_text()
        for m in re.finditer(lib_helpers, text, re.S | re.M):
            target, body = m.group(1), m.group(2)
            if target not in domain:
                continue
            libs_m = re.search(r"PUBLIC_LIBS\s*(.*?)(?=PRIVATE_LIBS|\Z)", body, re.S)
            if not libs_m:
                continue
            libs = re.findall(r"(pp_(?:base|domain)_[A-Za-z0-9_]+)", libs_m.group(1))
            for lib in libs:
                if lib not in domain or lib == target:
                    continue
                edge = f"{target}->{lib}"
                if edge not in legacy:
                    print(f"FAIL: new domain peer PUBLIC_LIBS edge {edge}")
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
