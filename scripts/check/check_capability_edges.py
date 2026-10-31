#!/usr/bin/env python3
"""Capability edges gate (projects/capability-repos P0; docs/architecture/SRC_LAYOUT.md § Capabilities).

Groups src/ by capability, counts `#include "…"` sites from one capability into another, and fails on any
edge the capability order does not allow unless it is in the allowlist with exactly that count. The
allowlist holds the edges still to be cut: a new one, or one that grows, fails; one that shrinks fails too,
with the line to lower, so the list stays true as edges are cut.

Usage: scripts/check/check_capability_edges.py [--list]   (--list prints every cross-capability edge)
"""

from __future__ import annotations

import os
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
ALLOWLIST = Path(__file__).with_name("capability_edges_allowlist.txt")

# Capability → its paths under src/ (the longest matching prefix wins). DESIGN § Capabilities in the tree.
GROUPS: dict[str, list[str]] = {
    # Mechanisms every capability uses (runtime core and crypto leave for pp-cpp-common / pp-cpp-crypto, P2).
    "base": ["common", "foundation"],
    # Identity (CR008): keys, cards, contacts, devices, the locker format.
    "people": ["lib/people", "domain/peers", "common/directory"],
    # The Brief backend client (CR008): registration, directory, blobs, push. Deployment-specific.
    "net": ["lib/net", "domain/registration"],
    # Public posts and the content store they share with attachments (CR009). Never above messaging.
    "social": ["lib/posts", "lib/content"],
    # Messaging, with pricing inside it (CR008): records, delivery, attachments, the conversations hub.
    "messaging": [
        "lib/messaging", "lib/pricing", "common/chat", "common/thread",
        "domain/delivery", "domain/attachments", "feature/conversations",
    ],
    "calls": ["lib/calls", "lib/media", "common/media", "domain/call_media", "feature/calls"],
    "broadcast": ["lib/broadcast", "feature/broadcast"],
    "node": ["domain/node", "app/node"],
    # The app: shell, GUI, composition — and AI, the shell's planner rather than a capability (CR007).
    "app": ["app", "gui", "feature/product", "feature/settings", "lib/ui", "lib/ai", "feature/ai", "common/ui"],
}

# What each capability may include besides itself. DESIGN § Target.
ALLOWED: dict[str, set[str]] = {
    "base": set(),
    "people": {"base"},
    "net": {"base", "people"},
    "social": {"base", "people"},
    "messaging": {"base", "people", "net", "social"},
    "calls": {"base", "people", "net", "social", "messaging"},
    "broadcast": {"base", "people", "net", "social", "messaging", "calls"},
    "node": {"base", "people", "net"},
    "app": set(GROUPS),
}

# Paths no capability owns: tools beside the node (probes, linked on their own).
IGNORED = ["app/node/tools"]

# Directories under these roots must each belong to a capability, so a new one cannot bypass the gate.
MAPPED_ROOTS = ["lib", "domain", "feature"]

SOURCE_SUFFIXES = (".h", ".hpp", ".c", ".cc", ".cpp", ".cxx", ".mm")
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def capability_of(rel: str) -> str | None:
    if any(rel == p or rel.startswith(p + "/") for p in IGNORED):
        return None
    best: tuple[str, int] | None = None
    for group, prefixes in GROUPS.items():
        for prefix in prefixes:
            if (rel == prefix or rel.startswith(prefix + "/")) and (best is None or len(prefix) > best[1]):
                best = (group, len(prefix))
    return best[0] if best else None


def unmapped_dirs() -> list[str]:
    missing = []
    for root in MAPPED_ROOTS:
        for child in sorted((SRC / root).iterdir()):
            rel = f"{root}/{child.name}"
            if child.is_dir() and capability_of(rel) is None and rel not in IGNORED:
                missing.append(rel)
    return missing


def count_edges() -> tuple[Counter, dict[tuple[str, str], list[str]]]:
    edges: Counter = Counter()
    sites: dict[tuple[str, str], list[str]] = {}
    for dirpath, _dirs, files in os.walk(SRC):
        for name in files:
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            path = Path(dirpath) / name
            rel = path.relative_to(SRC).as_posix()
            source = capability_of(rel)
            if source is None:
                continue
            for number, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
                match = INCLUDE.match(line)
                if not match:
                    continue
                target = capability_of(match.group(1))
                if target is None or target == source:
                    continue
                edges[(source, target)] += 1
                sites.setdefault((source, target), []).append(f"src/{rel}:{number}: {match.group(1)}")
    return edges, sites


def read_allowlist() -> Counter:
    allowed: Counter = Counter()
    for raw in ALLOWLIST.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        match = re.fullmatch(r"(\w+)\s*->\s*(\w+)\s+(\d+)", line)
        if not match:
            sys.exit(f"{ALLOWLIST.name}: cannot read '{raw}' (want: from -> to count)")
        allowed[(match.group(1), match.group(2))] = int(match.group(3))
    return allowed


def main() -> int:
    missing = unmapped_dirs()
    if missing:
        for rel in missing:
            print(f"FAIL: src/{rel} belongs to no capability; add it to GROUPS in {Path(__file__).name}")
        return 1

    edges, sites = count_edges()
    if "--list" in sys.argv:
        for (source, target), n in sorted(edges.items(), key=lambda item: -item[1]):
            mark = "" if target in ALLOWED[source] else "   (against the order)"
            print(f"{source} -> {target} {n}{mark}")
        return 0

    allowlist = read_allowlist()
    failed = False
    for (source, target), n in sorted(edges.items()):
        if target in ALLOWED[source]:
            continue
        allowed = allowlist.get((source, target), 0)
        if n > allowed:
            failed = True
            print(f"FAIL: {source} -> {target}: {n} include sites, {allowed} allowed. "
                  "Capabilities include only those below them (projects/capability-repos DESIGN § Target):")
            for site in sites[(source, target)]:
                print(f"  {site}")
    for (source, target), allowed in sorted(allowlist.items()):
        n = edges.get((source, target), 0)
        if target in ALLOWED.get(source, set()):
            failed = True
            print(f"FAIL: {source} -> {target} is allowed by the order; drop it from {ALLOWLIST.name}")
        elif n < allowed:
            failed = True
            print(f"FAIL: {source} -> {target} is down to {n} (allowlist says {allowed}); "
                  f"set it to {n} in {ALLOWLIST.name}" + (" — or drop the line" if n == 0 else ""))
    if failed:
        return 1
    print(f"capability edges: ok ({sum(allowlist.values())} include sites left to cut)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
