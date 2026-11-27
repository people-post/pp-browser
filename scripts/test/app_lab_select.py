"""App-lab scenario tags: the vocabulary, choosing scenarios by tag, and the tags a change touches.

pp_app_lab.py declares each scenario with its tags (its SCENARIOS list); this module holds what the tags mean and
how a run picks scenarios (docs/ops/TEST_STRATEGY.md § Scenario tags; release-compatibility RC013):

  --only NAME     that scenario, whatever else (opt-in ones included)
  --tags a,b      every scenario carrying a or b (that this run's mode runs: no opt-in, no NAT-only in --suite app)
  --changed SPEC  the tags of the paths SPEC names (PATH_RULES), then as --tags

The three add up (a scenario runs when any of them picks it). With none of them a run takes every scenario its mode
runs, as before.

A scenario carries the tags of what it checks, not of everything it passes through: every scenario registers (net)
and holds keys (people), and a call scenario sends a message or two. --changed widens a change below a capability to
the capabilities above it, as CI's capability filters do, so a people change still picks the call scenarios.

No dependency outside the standard library; app_lab_select_test.py tests it (python3 -m unittest).
"""

import difflib
import fnmatch
import os
import re
import subprocess

# One tag per capability folder (capabilities/<name>/, CTest label capability:<name>, build.yml's `changes`
# filters). The base (capabilities/base) is below all of them and has no tag: a change there picks everything.
CAPABILITY_TAGS = {
    "people": "identity, contacts, account cards and keys, the directory",
    "net": "the relay client and registration",
    "social": "posts and content",
    "delivery": "sessions, envelopes, the control lane, the outbox: a message reaching each device",
    "pricing": "pricing and credits",
    "media": "devices, codecs, the media engine: what a call sounds and looks like",
    "messaging": "chats: messages, the inbox, a group's chat and roster, attachments, voice messages",
    "calls": "calls, 1:1 and group",
    "broadcast": "live broadcast",
    "node": "pp-node, which the lab's seeds run (hop, hop2): parks, circuits, hosting group calls",
    "relay": "relays: the org's (pp-relay-apis) and, from node-relay NR5, the message relay on pp-node-plus "
    "(capabilities/relay): mailboxes, the directory, lockers, two relays",
}
# Tags across capabilities, for the questions a capability does not answer.
LAB_TAGS = {
    "mesh": "reach between users: seeds, parking, circuits, NATs, a network that drops or changes",
    "groups": "group chats and group calls",
    "devices": "one account on several devices: linking, signing out, restoring, the recovery phrase",
    "push": "the app in the background: push wakes, staying connected",
    "smoke": "the lab-smoke set: the release-critical paths in a few minutes (every merge to main)",
}
VOCABULARY = {**CAPABILITY_TAGS, **LAB_TAGS}

# What each capability sits on (SRC_LAYOUT § Capabilities; the base under all). Mirrors build.yml's `changes`
# filters, where each capability's filter is the ones below it plus its folder; app_lab_select_test.py checks the
# two agree.
BELOW = {
    "people": (),
    "net": ("people",),
    "social": ("people",),
    "delivery": ("net",),
    "pricing": ("delivery",),
    "media": (),
    "messaging": ("pricing", "social", "media"),
    "calls": ("pricing", "media"),
    "broadcast": ("calls",),
    "node": ("net",),
    "relay": ("net",),
}

ALL = "all"
NONE = "none"


def below(capability):
    """Every capability `capability` sits on, transitively (not the base, not itself)."""
    seen = set()
    stack = list(BELOW[capability])
    while stack:
        c = stack.pop()
        if c not in seen:
            seen.add(c)
            stack.extend(BELOW[c])
    return seen


def above(capability):
    """`capability` and every capability that sits on it: what a change in its folder can break."""
    return {c for c in BELOW if c == capability or capability in below(c)}


# Path → tags, first match wins (fnmatch: `*` crosses `/`). `capabilities/<x>/*` is the one rule with a variable:
# x and the capabilities above it. A path in no rule picks everything (unknown is not safe to skip).
PATH_RULES = [
    # The lab itself: its driver, its stack and its probe.
    (("scripts/test/pp_app_lab.py", "scripts/test/app_lab_select.py", "scripts/test/pp_local_test.sh",
      "packaging/pp-node/docker-compose.app-lab*", "packaging/pp-node/app-lab-*",
      "src/app/node/tools/app_probe/*"), ALL, "the app lab and its probe"),
    ((".github/workflows/lab-run.yml", ".github/workflows/lab-smoke.yml", ".github/workflows/lab-nightly.yml"),
     ("smoke",), "the lab's workflows"),
    (("docs/*", "projects/*", "design/*", "reports/*", "*.md", "LICENSE*"), NONE, "docs"),
    (("*/tests/*", "*/testing/*", "scripts/test/*"), NONE, "unit tests and other suites (not the app lab)"),
    (("src/gui/*", "src/lib/ui/*", "src/common/ui/*", "src/lib/ai/*", "src/feature/ai/*", "src/lib/scan/*",
      "assets/*"), NONE, "GUI and AI (pp-app-probe has no GUI)"),
    (("android/*", "ios/*", "packaging/ios/*", "packaging/macos/*", "scripts/*", ".github/*", ".cursor/*",
      ".vscode/*", ".clang-format", ".gitignore", ".git-blame-ignore-revs", "config.json.example",
      ".exmaple-config.json"), NONE, "other platforms, tooling and CI (not on the lab's Linux build)"),
    (("capabilities/base/*", "cmake/*", "CMakeLists.txt", "src/CMakeLists.txt", "third_party/*"), ALL,
     "the base, the build and the pinned libraries (pp-cpp-*)"),
    (("capabilities/*",), "capability", "a capability folder: it and the capabilities above it"),
    (("src/feature/*", "src/foundation/*", "src/lib/storage/*", "src/domain/*", "src/app/Bootstrap.*"), ALL,
     "the app stack pp-app-probe runs (ProductStack, presenters, storage, config)"),
    (("src/app/node/*", "packaging/pp-node/Dockerfile", "packaging/pp-node/config.json.example"), ("node",),
     "pp-node, the lab's seeds"),
    (("packaging/pp-node/Dockerfile.hard-*", "packaging/pp-node/hard-gw-*"), ("mesh",),
     "the NAT gateways (--suite app-nat)"),
    (("src/app/*", "packaging/pp-node/*"), NONE, "the desktop app's shell and other node packaging"),
]


def tags_for_path(path):
    """(tags, rule): the tags `path` (repository-relative) picks and the rule that said so. tags is a set; every
    tag of the vocabulary for an ALL rule, empty for NONE."""
    path = path.strip()
    if path.startswith("./"):
        path = path[2:]
    for patterns, tags, why in PATH_RULES:
        if not any(fnmatch.fnmatchcase(path, p) for p in patterns):
            continue
        if tags == ALL:
            return set(VOCABULARY), why
        if tags == NONE:
            return set(), why
        if tags == "capability":
            name = path.split("/")[1] if path.count("/") >= 1 else ""
            if name not in BELOW:
                return set(VOCABULARY), "capabilities/%s is no capability the lab knows: everything" % name
            return above(name), "capabilities/%s and what sits on it" % name
        return set(tags), why
    return set(VOCABULARY), "not in the table: everything"


def tags_for_paths(paths):
    """(tags, lines): the union of every path's tags, and one line per rule that matched, saying how many paths
    and which tags (what --changed prints)."""
    tags, by_rule = set(), {}
    for path in paths:
        t, why = tags_for_path(path)
        tags |= t
        entry = by_rule.setdefault(why, [t, []])
        entry[0] = entry[0] | t
        entry[1].append(path)
    lines = []
    for why, (t, hit) in by_rule.items():
        named = ", ".join(hit[:3]) + (", …" if len(hit) > 3 else "")
        lines.append("%d path%s (%s): %s → %s" % (len(hit), "" if len(hit) == 1 else "s", named, why, describe(t)))
    return tags, lines


def describe(tags):
    if not tags:
        return "none"
    if tags >= set(VOCABULARY):
        return "all"
    return ", ".join(sorted(tags))


def _git(root, *args):
    r = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("--changed: git %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return [line for line in r.stdout.splitlines() if line.strip()]


def _is_rev(root, item):
    r = subprocess.run(["git", "-C", root, "rev-parse", "--verify", "--quiet", item + "^{commit}"],
                       capture_output=True, text=True)
    return r.returncode == 0


def changed_paths(spec, root, stdin=None):
    """The paths `spec` names (what --changed takes), relative to the repository `root`:
      - a git range (`A..B`, `A...B`): the files it changes;
      - one revision (`origin/main`): the files changed since it and HEAD forked — commits, the working tree and new
        untracked files (what this branch changes);
      - `-`: paths on stdin, one per line;
      - anything else: a path (several separated by commas or spaces)."""
    paths = []
    for item in [s for s in re.split(r"[,\s]+", spec) if s]:
        if item == "-":
            paths += [line.strip() for line in (stdin or []) if line.strip()]
        elif ".." in item and not os.path.exists(os.path.join(root, item)):
            paths += _git(root, "diff", "--name-only", item)
        elif not os.path.exists(os.path.join(root, item)) and _is_rev(root, item):
            base = _git(root, "merge-base", item, "HEAD")[0]
            paths += _git(root, "diff", "--name-only", base)
            paths += _git(root, "ls-files", "--others", "--exclude-standard")
        else:
            paths.append(item)
    seen = set()
    return [p for p in paths if not (p in seen or seen.add(p))]


def parse_tags(values):
    """--tags values (repeatable, comma-separated) as a set; an unknown tag is an error naming the close ones."""
    tags = {t for v in values for t in re.split(r"[,\s]+", v) if t}
    unknown = sorted(tags - set(VOCABULARY))
    if unknown:
        raise SystemExit("unknown tag%s %s (tags: %s)" % ("" if len(unknown) == 1 else "s", ", ".join(unknown),
                                                          ", ".join(VOCABULARY)))
    return tags


def check_registry(entries):
    """The self-check every run makes: entries are (name, tags); returns the problems (empty: fine). Each scenario
    has a unique name, only tags of the vocabulary, and at least one capability tag (--changed works through those:
    a scenario without one is never picked by a change); the smoke set is not empty."""
    problems, seen = [], set()
    for name, tags in entries:
        if name in seen:
            problems.append("%s: declared twice" % name)
        seen.add(name)
        unknown = sorted(set(tags) - set(VOCABULARY))
        if unknown:
            problems.append("%s: unknown tag %s (the vocabulary is in app_lab_select.py)" % (name, ", ".join(unknown)))
        if not set(tags) & set(CAPABILITY_TAGS):
            problems.append("%s: no capability tag (one of %s)" % (name, ", ".join(CAPABILITY_TAGS)))
    if not any("smoke" in tags for _, tags in entries):
        problems.append("no scenario is tagged smoke")
    return problems


def select(entries, skips, only=(), tags=(), changed=None):
    """Which scenarios a run takes, and why. entries: (name, tags) in run order; skips: name → why this mode does not
    run it unless named; only: names; tags: a set; changed: the set of tags --changed found (None: not given).
    Returns [(name, tags, selected, why)] for every scenario."""
    names = [n for n, _ in entries]
    unknown = [n for n in only if n not in names]
    if unknown:
        hints = {n: difflib.get_close_matches(n, names, 3) for n in unknown}
        raise SystemExit("no scenario named " + "; ".join(
            "%s%s" % (n, " (did you mean %s?)" % ", ".join(h) if h else "") for n, h in hints.items()))
    by_name = bool(only) or bool(tags) or changed is not None
    out = []
    for name, own in entries:
        own = set(own)
        if name in only:
            out.append((name, own, True, "--only"))
            continue
        reasons = []
        if own & set(tags):
            reasons.append("--tags " + ",".join(sorted(own & set(tags))))
        if changed and own & changed:
            reasons.append("--changed (everything)" if changed >= set(VOCABULARY) else
                           "--changed " + ",".join(sorted(own & changed)))
        if by_name and not reasons:
            out.append((name, own, False, "not picked"))
        elif name in skips:
            picked = " (picked by %s)" % "; ".join(reasons) if reasons else ""
            out.append((name, own, False, skips[name] + picked))
        else:
            out.append((name, own, True, "; ".join(reasons) or "default run"))
    return out


def format_selection(rows, everything=False):
    """The lines a run prints before it starts: each selected scenario, its tags and why. With `everything` (--list)
    also the ones this mode leaves out, and why; the ones no --only / --tags / --changed picked are only counted."""
    width = max([len(n) for n, _, _, _ in rows] + [10])
    lines, not_picked = [], 0
    for name, tags, selected, why in rows:
        if why == "not picked":
            not_picked += 1
        elif selected or everything:
            lines.append("  %-4s %-*s [%s] %s" % ("run" if selected else "-", width, name, " ".join(sorted(tags)), why))
    count = sum(1 for r in rows if r[2])
    lines.append("%d of %d scenarios selected%s" % (count, len(rows),
                                                   " (%d not picked)" % not_picked if not_picked else ""))
    return lines
