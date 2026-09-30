#!/usr/bin/env bash
# Guard the calls owner's single runner (docs/architecture/THREADING.md § Calls owner).
#
# Only CallStack runs the calls thread: it owns the event loop (CallsLoop over CallsExecutor /
# CallsThread) and is the only place that posts, schedules or waits on an owner. Every other type
# under src/feature/calls is a passive component: it reports through its CallsOutbox and gets
# timers from it. CallUiBackend is the UI edge and may post replies to UI. Tests are exempt.
#
# EXCEPTIONS lists known, documented sites with a count: a new site fails, and so does a removed one
# (drop it from the list). Empty today.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
calls = root / "src" / "feature" / "calls"

# The runner itself: the owner thread, its executor, the loop and the stack that owns them.
RUNNER = {"CallsThread", "CallsExecutor", "CallsLoop", "CallStack"}

# The owner's thread machinery, and scheduling / waiting on any thread.
FORBIDDEN = re.compile(
    r"\bCallsThread::|\bCallsTasks\b|\bCallsOwnerExecutor\s*\(|"
    r"\bAppRuntime::(PostTo|PostToFront|PostToOwnerOrRun|ScheduleOn|ScheduleCoordinatorOneShot|"
    r"CancelCoordinatorTimer|PostWorker\w*|PostCoordinator\w*|RunAndWait)\b|"
    r"\bstd::thread\b|\bsleep_for\b"
)
UI_POST = re.compile(r"\bAppRuntime::PostUI\b")
UI_EDGE = {"CallUiBackend"}

# path (relative to src/feature/calls) -> (count, why)
EXCEPTIONS = {}

fail = 0
counts = {}
for path in sorted(calls.rglob("*")):
    if path.suffix not in (".h", ".cpp") or "tests" in path.relative_to(calls).parts:
        continue
    rel = path.relative_to(calls).as_posix()
    if path.stem in RUNNER:
        continue
    for n, line in enumerate(path.read_text().splitlines(), 1):
        code = line.split("//", 1)[0]
        hit = FORBIDDEN.search(code) or (UI_POST.search(code) and path.stem not in UI_EDGE)
        if not hit:
            continue
        counts[rel] = counts.get(rel, 0) + 1
        if rel not in EXCEPTIONS:
            print(f"FAIL: src/feature/calls/{rel}:{n}: {hit.group(0)} — only CallStack schedules on the calls owner;"
                  " report through the component's CallsOutbox instead")
            fail = 1

for rel, (expected, why) in EXCEPTIONS.items():
    got = counts.get(rel, 0)
    if got != expected:
        print(f"FAIL: src/feature/calls/{rel}: {got} owner-thread site(s), {expected} expected ({why});"
              " update EXCEPTIONS in scripts/check/check_calls_owner.sh")
        fail = 1

if fail:
    sys.exit(1)
print("check_calls_owner: OK")
PY
