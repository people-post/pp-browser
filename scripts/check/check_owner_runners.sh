#!/usr/bin/env bash
# Guard the owner runners (docs/architecture/THREADING.md § Owner runners).
#
# Each component tree on an owner thread has one runner, its root: the only type in the tree that
# posts, schedules or waits. Every other type in the tree is a passive component: it reports
# through its OwnerOutbox and gets timers from it. Tests are exempt.
#
#   feature/calls      runner: CallStack (with its loop: CallsLoop / CallsExecutor / CallsThread);
#                      CallUiBackend is the UI edge and may post replies to UI.
#   feature/broadcast  runner: BroadcastHub.
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

# tree (under src/) -> (runner file stems, UI edge stems)
TREES = {
    "feature/calls": ({"CallsThread", "CallsExecutor", "CallsLoop", "CallStack"}, {"CallUiBackend"}),
    "feature/broadcast": ({"BroadcastHub"}, set()),
}

# Owner-thread machinery, and scheduling / waiting on any thread.
FORBIDDEN = re.compile(
    r"\bCallsThread::|\bOwnerTasks\b|\bCallsOwnerExecutor\s*\(|\bDeferredSelf\b(?!::Token|::Alive|\.h)|"
    r"\bAppRuntime::(PostTo|PostToFront|PostToOwnerOrRun|ScheduleOn|ScheduleCoordinatorOneShot|"
    r"CancelCoordinatorTimer|PostWorker\w*|PostCoordinator\w*|RunAndWait)\b|"
    r"\bstd::thread\b|\bsleep_for\b"
)
UI_POST = re.compile(r"\bAppRuntime::PostUI\b")

# path (relative to src/) -> (count, why)
EXCEPTIONS = {}

fail = 0
counts = {}
for tree, (runner, ui_edge) in TREES.items():
    base = root / "src" / tree
    for path in sorted(base.rglob("*")):
        if path.suffix not in (".h", ".cpp") or "tests" in path.relative_to(base).parts:
            continue
        if path.stem in runner:
            continue
        rel = path.relative_to(root / "src").as_posix()
        for n, line in enumerate(path.read_text().splitlines(), 1):
            code = line.split("//", 1)[0]
            hit = FORBIDDEN.search(code) or (UI_POST.search(code) and path.stem not in ui_edge)
            if not hit:
                continue
            counts[rel] = counts.get(rel, 0) + 1
            if rel not in EXCEPTIONS:
                print(f"FAIL: src/{rel}:{n}: {hit.group(0)} — only the tree's runner posts / schedules / waits;"
                      " report through the component's OwnerOutbox instead")
                fail = 1

for rel, (expected, why) in EXCEPTIONS.items():
    got = counts.get(rel, 0)
    if got != expected:
        print(f"FAIL: src/{rel}: {got} owner-thread site(s), {expected} expected ({why});"
              " update EXCEPTIONS in scripts/check/check_owner_runners.sh")
        fail = 1

if fail:
    sys.exit(1)
print("check_owner_runners: OK")
PY
