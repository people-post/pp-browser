#!/usr/bin/env bash
# Guard include edges between domain/mesh folder libraries.
#
# The allowed edges are the DEPS of each `_pp_mesh_library(...)` in src/domain/mesh/CMakeLists.txt
# (transitively), so the link graph and the include graph cannot drift apart: add the DEPS entry,
# or do not add the include. Folders map to libraries as: shared/ and l4/shared/ -> shared,
# l4/<proto>/ -> <proto>, <folder>/ -> <folder>. Tests are exempt.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
mesh = root / "src" / "domain" / "mesh"
cmake = (mesh / "CMakeLists.txt").read_text()

deps = {}
for m in re.finditer(r"_pp_mesh_library\(\s*(\w+)(.*?)\)", cmake, re.S):
    name, body = m.group(1), m.group(2)
    d = re.search(r"DEPS\s+([\w\s]+)$", body.strip(), re.S)
    deps[name] = set(d.group(1).split()) if d else set()

def closure(name, seen=None):
    seen = set() if seen is None else seen
    for dep in deps.get(name, ()):
        if dep not in seen:
            seen.add(dep)
            closure(dep, seen)
    return seen

allowed = {name: closure(name) | {name} for name in deps}

def lib_of(rel):
    parts = rel.split("/")
    if parts[0] == "shared" or parts[:2] == ["l4", "shared"]:
        return "shared"
    if parts[0] == "l4":
        return parts[1]
    return parts[0]

fail = 0
for path in sorted(mesh.rglob("*")):
    if path.suffix not in (".h", ".cpp") or "tests" in path.relative_to(mesh).parts:
        continue
    rel = path.relative_to(mesh).as_posix()
    src = lib_of(rel)
    if src not in allowed:
        print(f"FAIL: {rel}: folder has no _pp_mesh_library ({src})")
        fail = 1
        continue
    for n, line in enumerate(path.read_text().splitlines(), 1):
        m = re.match(r'\s*#include "domain/mesh/([^"]+)"', line)
        if not m:
            continue
        dst = lib_of(m.group(1))
        if dst not in allowed[src]:
            print(f"FAIL: {rel}:{n}: mesh {src} -> {dst} is not a DEPS edge")
            print(f"  {line.strip()}")
            fail = 1
# pp-cpp-mesh extraction (docs/architecture/MESH.md): the mesh includes no pp-browser header but its
# own — external libraries (pp-cpp-common, pp-cpp-amp, pp-cpp-crypto) are fine. These still do;
# remove an entry when its last include goes (a stale entry fails too), and never add one.
LEGACY_APP_INCLUDES = {
    "common/chat/IDirectMessageClient.h",
    "common/CodedFailure.h",
    "common/directory/DirectoryTypes.h",
    "common/directory/IDirectoryClient.h",
    "common/directory/MeshHopDial.h",
    "common/directory/MeshHopTypes.h",
    "common/directory/RelayScope.h",
    "common/LengthPrefixedCodec.h",
    "common/media/CallMediaHealth.h",
    "common/media/MediaChannel.h",
    "common/metrics/MetricsRegistry.h",
    "common/net/PublicAddress.h",
    "common/PbrCompat.h",
    "common/privacy/AddressDisclosure.h",
    "common/SettledWait.h",
    "common/thread/ChatBlobTypes.h",
    "common/ValueJson.h",
    "foundation/crypto/CryptoConstants.h",
    "foundation/crypto/CryptoTypes.h",
    "foundation/crypto/CryptoUtil.h",
    "foundation/crypto/MessageCipher.h",
    "foundation/crypto/MlDsa.h",
    "foundation/data/AtomicFileWrite.h",
    "foundation/data/MeshConfigTypes.h",
    "foundation/identity/MultiaddrPeerId.h",
    "foundation/identity/PeerIdUtil.h",
}
src = root / "src"
used = set()
for path in sorted(mesh.rglob("*")):
    if path.suffix not in (".h", ".cpp") or "tests" in path.relative_to(mesh).parts:
        continue
    rel = path.relative_to(mesh).as_posix()
    for n, line in enumerate(path.read_text().splitlines(), 1):
        m = re.match(r'\s*#include "([^"]+)"', line)
        if not m or m.group(1).startswith("domain/mesh/") or not (src / m.group(1)).is_file():
            continue
        if m.group(1) in LEGACY_APP_INCLUDES:
            used.add(m.group(1))
            continue
        print(f"FAIL: {rel}:{n}: mesh includes the app's {m.group(1)} (not on LEGACY_APP_INCLUDES)")
        fail = 1
for stale in sorted(LEGACY_APP_INCLUDES - used):
    print(f"FAIL: LEGACY_APP_INCLUDES lists {stale}, which the mesh no longer includes: drop the entry")
    fail = 1
sys.exit(fail)
PY

echo "OK: domain/mesh layer edges; $(grep -c '^    "' "$0") app headers still to peel (LEGACY_APP_INCLUDES)"
