#!/usr/bin/env python3
"""Capability edges gate (projects/capability-repos P0; docs/architecture/SRC_LAYOUT.md § Capabilities).

Groups src/ by capability, counts `#include "…"` sites from one capability into another, and fails on any
edge the capability order does not allow unless it is in the allowlist with exactly that count. The
allowlist holds the edges still to be cut: a new one, or one that grows, fails; one that shrinks fails too,
with the line to lower, so the list stays true as edges are cut.

Links count too (CR016): every pp-browser target a src/ CMakeLists.txt names (PUBLIC_LIBS, LINK_LIBS,
target_link_libraries) is a link from that folder's capability to the target's, held to the same order;
the allowlist names those as "link from -> to count".

Usage: scripts/check/check_capability_edges.py [--list | --lib-edges]
  --list       prints every cross-capability edge (includes, then links)
  --lib-edges  prints the src/lib module pairs one may include from the other ("from->to"): different
               capabilities, in the order (CR013) — what check_base_includes.sh lets lib peers include
"""

from __future__ import annotations

import os
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
# Capabilities moved out of src/ (CR022): capabilities/<name>/src mirrors the tree, so paths under it read as
# they did under src/.
CAPABILITIES = ROOT / "capabilities"


def source_roots() -> list[tuple[Path, str | None]]:
    """(root, the capability its folder is) for src/ and every capabilities/<name>/src."""
    roots: list[tuple[Path, str | None]] = [(SRC, None)]
    if CAPABILITIES.is_dir():
        for folder in sorted(CAPABILITIES.iterdir()):
            if (folder / "src").is_dir():
                roots.append((folder / "src", folder.name))
    return roots
ALLOWLIST = Path(__file__).with_name("capability_edges_allowlist.txt")

# Capability → its paths under src/ (the longest matching prefix wins). DESIGN § Capabilities in the tree.
GROUPS: dict[str, list[str]] = {
    # Mechanisms every capability uses (runtime core and crypto leave for pp-cpp-common / pp-cpp-crypto, P2).
    "base": ["common", "foundation"],
    # The product's own base (rule 1, CR015): config and its file format, data paths, platform hooks, user-facing
    # text. The apps read it and pass capabilities values and ports.
    "product": ["foundation/data", "foundation/platform", "foundation/i18n", "foundation/product"],
    # Identity (CR008): keys, cards, contacts, devices, the locker format.
    "people": ["lib/people", "domain/peers", "common/directory"],
    # The Brief backend client (CR008): registration, directory, blobs, push. Deployment-specific.
    "net": ["lib/net", "domain/registration"],
    # Public posts and the content store they share with attachments (CR009). Never above messaging.
    "social": ["lib/posts", "lib/content"],
    # Secure delivery (CR020): sessions, envelopes and their codecs, the control lane, the outbox, the relay's
    # message client, and the records they carry (threads, messages, channels) with the store ports.
    "delivery": ["lib/delivery", "common/chat", "common/thread", "domain/delivery"],
    # Initiation pricing and payment promises: what contacting someone costs, for messages and calls alike.
    "pricing": ["lib/pricing"],
    # Devices, hardware codecs and the media engine every realtime feature runs on (CR023 step 2, L016).
    "media": ["lib/media", "common/media_engine"],
    # Chat on top of delivery (CR008, CR020): the stores, inbox, groups, attachments, history, the hub; voice messages.
    "messaging": ["lib/messaging", "lib/voice", "domain/attachments", "feature/conversations"],
    "calls": ["lib/calls", "lib/call_audio", "common/media", "domain/call_media", "feature/calls"],
    "broadcast": ["lib/broadcast", "feature/broadcast"],
    "node": ["domain/node"],
    # pp-node, the headless app (CR017): an app, so it reads the product's base; it picks people, net and node.
    "node_app": ["app/node"],
    # The app: shell, GUI, composition — and AI, the shell's planner rather than a capability (CR007).
    "app": ["app", "gui", "feature/product", "feature/presenters", "feature/settings", "lib/ui", "lib/ai", "lib/storage", "lib/scan", "feature/ai", "common/ui"],
}

# What each capability may include besides itself. DESIGN § Target.
ALLOWED: dict[str, set[str]] = {
    "base": set(),
    "people": {"base"},
    "net": {"base", "people"},
    "social": {"base", "people"},
    "delivery": {"base", "people", "net"},
    "pricing": {"base", "people", "net", "delivery"},
    "media": {"base"},
    "messaging": {"base", "people", "net", "social", "delivery", "pricing", "media"},
    # Realtime rides delivery, not chat (CR020): call history reaches the records through delivery's store port.
    "calls": {"base", "people", "net", "delivery", "pricing", "media"},
    "broadcast": {"base", "people", "net", "delivery", "pricing", "media", "calls"},
    "node": {"base", "people", "net"},
    "product": {"base"},
    "node_app": {"base", "product", "people", "net", "node"},
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
    for base, _folder in source_roots():
        for root in MAPPED_ROOTS:
            if not (base / root).is_dir():
                continue
            for child in sorted((base / root).iterdir()):
                rel = f"{root}/{child.name}"
                if child.is_dir() and capability_of(rel) is None and rel not in IGNORED:
                    missing.append(rel)
    return missing


def misplaced_files() -> list[str]:
    """Files in capabilities/<name>/src whose path belongs to another capability (or to none)."""
    wrong = []
    for base, folder in source_roots():
        if folder is None:
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file():
                rel = path.relative_to(base).as_posix()
                owner = capability_of(rel)
                if owner != folder:
                    wrong.append(f"{path.relative_to(ROOT).as_posix()} belongs to {owner or 'no capability'}")
    return wrong


def count_edges() -> tuple[Counter, dict[tuple[str, str], list[str]]]:
    edges: Counter = Counter()
    sites: dict[tuple[str, str], list[str]] = {}
    for base, _folder in source_roots():
        count_edges_under(base, edges, sites)
    return edges, sites


def count_edges_under(base: Path, edges: Counter, sites: dict[tuple[str, str], list[str]]) -> None:
    for dirpath, _dirs, files in os.walk(base):
        for name in files:
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            path = Path(dirpath) / name
            rel = path.relative_to(base).as_posix()
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
                where = path.relative_to(ROOT).as_posix()
                sites.setdefault((source, target), []).append(f"{where}:{number}: {match.group(1)}")


TARGET_DEF = re.compile(
    r"\b(?:pp_browser_add_\w+_library|add_library|add_executable)\(\s*([A-Za-z0-9_\-]+)")
TARGET_NAME = re.compile(r"\b(pp[_-][A-Za-z0-9_\-]+)")
QUOTED = re.compile(r'"[^"]*"')
# Convenience bundles (src/CMakeLists.txt, src/lib/CMakeLists.txt): everything below the app, so the app's.
BUNDLES = {"pp_foundation", "pp_lib", "pp_base"}


def cmake_text(path: Path) -> str:
    lines = (line.split("#", 1)[0] for line in path.read_text(errors="ignore").splitlines())
    return "\n".join(QUOTED.sub('""', line) for line in lines)


def cmake_files() -> list[tuple[str, str, str]]:
    """(where, capability, text without comments) for every CMakeLists.txt a capability owns."""
    files = []
    for base, folder in source_roots():
        for path in sorted(base.rglob("CMakeLists.txt")):
            rel = path.relative_to(base).as_posix()
            if capability_of(rel) is None:
                continue
            files.append((path.relative_to(ROOT).as_posix(), capability_of(rel), cmake_text(path)))
        if folder is not None and (base.parent / "CMakeLists.txt").is_file():
            # The folder's own CMakeLists.txt (its common layer, the include root) is the capability's.
            top = base.parent / "CMakeLists.txt"
            files.append((top.relative_to(ROOT).as_posix(), folder, cmake_text(top)))
    return files


def count_links() -> tuple[Counter, dict[tuple[str, str], list[str]]]:
    """Links between capabilities: a target named in one capability's CMake that another capability defines."""
    files = cmake_files()
    owner: dict[str, str] = {name: "app" for name in BUNDLES}
    for _where, capability, text in files:
        for name in TARGET_DEF.findall(text):
            owner[name] = capability
    edges: Counter = Counter()
    sites: dict[tuple[str, str], list[str]] = {}
    for where, source, text in files:
        defined = set(TARGET_DEF.findall(text))
        for name in sorted(set(TARGET_NAME.findall(text)) - defined):
            target = owner.get(name)
            if target is None or target == source:
                continue
            edges[(source, target)] += 1
            sites.setdefault((source, target), []).append(f"{where}: {name}")
    return edges, sites


def read_allowlist() -> tuple[Counter, Counter]:
    """(include sites, link sites) still allowed per edge."""
    includes: Counter = Counter()
    links: Counter = Counter()
    for raw in ALLOWLIST.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        match = re.fullmatch(r"(link\s+)?(\w+)\s*->\s*(\w+)\s+(\d+)", line)
        if not match:
            sys.exit(f"{ALLOWLIST.name}: cannot read '{raw}' (want: [link] from -> to count)")
        (links if match.group(1) else includes)[(match.group(2), match.group(3))] = int(match.group(4))
    return includes, links


def check_against(kind: str, edges: Counter, sites: dict, allowlist: Counter) -> bool:
    """Fails on an edge against the order beyond its allowance, and on an allowance that is stale."""
    prefix = "link " if kind == "link" else ""
    failed = False
    for (source, target), n in sorted(edges.items()):
        if target in ALLOWED[source]:
            continue
        allowed = allowlist.get((source, target), 0)
        if n > allowed:
            failed = True
            print(f"FAIL: {prefix}{source} -> {target}: {n} {kind} sites, {allowed} allowed. "
                  "Capabilities use only those below them (projects/capability-repos DESIGN § Target):")
            for site in sites[(source, target)]:
                print(f"  {site}")
    for (source, target), allowed in sorted(allowlist.items()):
        n = edges.get((source, target), 0)
        if target in ALLOWED.get(source, set()):
            failed = True
            print(f"FAIL: {prefix}{source} -> {target} is allowed by the order; drop it from {ALLOWLIST.name}")
        elif n < allowed:
            failed = True
            print(f"FAIL: {prefix}{source} -> {target} is down to {n} (allowlist says {allowed}); "
                  f"set it to {n} in {ALLOWLIST.name}" + (" — or drop the line" if n == 0 else ""))
    return failed


def allowed_lib_edges() -> list[str]:
    """lib/<a> may include lib/<b> when they are different capabilities and the order allows it (CR013)."""
    modules = sorted({child.name for base, _folder in source_roots() if (base / "lib").is_dir()
                      for child in (base / "lib").iterdir() if child.is_dir()})
    pairs = []
    for source in modules:
        for target in modules:
            a, b = capability_of(f"lib/{source}"), capability_of(f"lib/{target}")
            if source != target and a and b and a != b and b in ALLOWED[a]:
                pairs.append(f"{source}->{target}")
    return pairs


def main() -> int:
    if "--lib-edges" in sys.argv:
        print("\n".join(allowed_lib_edges()))
        return 0
    missing = unmapped_dirs()
    if missing:
        for rel in missing:
            print(f"FAIL: {rel} belongs to no capability; add it to GROUPS in {Path(__file__).name}")
        return 1
    misplaced = misplaced_files()
    if misplaced:
        for line in misplaced:
            print(f"FAIL: {line}: a capability folder holds only its own paths (CR022)")
        return 1

    edges, sites = count_edges()
    links, link_sites = count_links()
    if "--list" in sys.argv:
        for kind, found in (("", edges), ("link ", links)):
            for (source, target), n in sorted(found.items(), key=lambda item: -item[1]):
                mark = "" if target in ALLOWED[source] else "   (against the order)"
                print(f"{kind}{source} -> {target} {n}{mark}")
        return 0

    allowed_includes, allowed_links = read_allowlist()
    failed = check_against("include", edges, sites, allowed_includes)
    failed = check_against("link", links, link_sites, allowed_links) or failed
    if failed:
        return 1
    print(f"capability edges: ok ({sum(allowed_includes.values())} include sites, "
          f"{sum(allowed_links.values())} link sites left to cut)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
