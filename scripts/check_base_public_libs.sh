#!/usr/bin/env bash
# Guard domain peer CMake PUBLIC_LIBS edges (North Star).
# Mirrors scripts/check_base_includes.sh legacy allowlist for link edges.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
domain = {
    "pp_base_people",
    "pp_base_messaging",
    "pp_base_net",
    "pp_base_mesh",
    "pp_base_media",
    "pp_base_ai",
    "pp_base_ai_conversation",
    "pp_base_ai_mcp",
    "pp_base_ui",
    "pp_base_render",
}
legacy = {
}

fail = 0
for cmake in sorted((root / "src" / "base").rglob("CMakeLists.txt")):
    text = cmake.read_text()
    for m in re.finditer(
        r"pp_browser_add_base_library\(\s*(pp_base_[A-Za-z0-9_]+)\s*(.*?)^\s*\)",
        text,
        re.S | re.M,
    ):
        target, body = m.group(1), m.group(2)
        if target not in domain:
            continue
        libs_m = re.search(r"PUBLIC_LIBS\s*(.*?)(?=PRIVATE_LIBS|\Z)", body, re.S)
        if not libs_m:
            continue
        libs = re.findall(r"(pp_base_[A-Za-z0-9_]+)", libs_m.group(1))
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
