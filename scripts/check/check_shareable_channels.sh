#!/usr/bin/env bash
# Guard the shareable channels (docs/contracts/DIAGNOSTICS.md): what Send diagnostics reads — the "Diag" and
# "Metrics" channels — is written only through DiagLine / MetricsLine, which take no free text. Streaming into
# those loggers directly (`DiagLog().info << name`, `getLogger("Diag")`, a module named "Diag") would bypass that.
#
# Allowed: the channel headers that own the loggers, the log-file installer that attaches their files, and tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
ALLOWED = {
    "src/common/Diag.h",
    "src/common/Metrics.h",
    "src/common/ShareableLine.h",
    "src/common/ShareableLine.cpp",
    "src/foundation/diagnostics/LogFile.cpp",
}
# The channel loggers themselves, or a logger named after a shareable channel.
FORBIDDEN = re.compile(
    r'\b(?:DiagLog|MetricsLog)\s*\(\s*\)|'
    r'\b(?:getLogger|redirectLogger|redirectTo)\s*\(\s*"(?:Diag|Metrics)"'
)

bad = []
for path in sorted((root / "src").rglob("*")):
    if path.suffix not in {".h", ".hpp", ".cpp", ".cc", ".mm", ".inc"}:
        continue
    rel = path.relative_to(root).as_posix()
    if rel in ALLOWED or "/tests/" in rel:
        continue
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if FORBIDDEN.search(line):
            bad.append(f"{rel}:{number}: {line.strip()}")

if bad:
    print("Write the Diag / Metrics channels only through DiagLine / MetricsLine (docs/contracts/DIAGNOSTICS.md):")
    print("\n".join(bad))
    sys.exit(1)
print("shareable channels: ok")
PY
