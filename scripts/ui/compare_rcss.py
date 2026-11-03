#!/usr/bin/env python3
"""Compare two RCSS files as multisets of (media, selector, property, value).

Hex case, #rgb -> #rrggbb (and #rgba -> #rrggbbaa), whitespace, comments, rule
order and selector-list formatting are ignored. Everything else must match.

Usage: compare_rcss.py A.rcss B.rcss     (exit 0 = identical, 1 = differences)
"""
import re
import sys
from collections import Counter

COLOR_RE = re.compile(r"#[0-9a-fA-F]{3,8}\b|rgba?\([^)]*\)")


def parse_rcss(text):
    """Return (rules, dropped_body_comments). A rule is a dict with keys
    media, selector (whitespace-collapsed), comments (list of raw comment
    strings placed before it) and decls (list of (prop, raw value))."""
    rules, comments, buf = [], [], []
    depth, media, cur, dropped, i, n = 0, "", None, 0, 0, len(text)

    def commit_decl():
        s = "".join(buf).strip()
        buf.clear()
        if not s:
            return
        prop, _, val = s.partition(":")
        cur["decls"].append((prop.strip(), " ".join(val.split())))

    while i < n:
        if text.startswith("/*", i):
            j = text.index("*/", i) + 2
            if depth == 1 and not "".join(buf).strip():
                comments.append(text[i:j])
            else:
                dropped += 1
            i = j
            continue
        c = text[i]
        if c == "{":
            head = " ".join("".join(buf).split())
            buf.clear()
            if depth == 0:
                media = head
            else:
                cur = {"media": media, "selector": head, "comments": comments, "decls": []}
                comments = []
            depth += 1
        elif c == "}":
            if depth == 2:
                commit_decl()
                rules.append(cur)
                cur = None
            else:
                buf.clear()
            depth -= 1
        elif c == ";" and depth == 2:
            commit_decl()
        else:
            buf.append(c)
        i += 1
    return rules, dropped


def norm_color(m):
    s = m.group(0).lower()
    if s.startswith("#") and len(s) in (4, 5):
        s = "#" + "".join(ch * 2 for ch in s[1:])
    elif s.startswith("rgb"):
        s = re.sub(r"\s+", "", s)
    return s


def norm_value(v):
    return " ".join(COLOR_RE.sub(norm_color, v).split())


def multiset(path):
    rules, _ = parse_rcss(open(path, encoding="utf-8").read())
    out = Counter()
    for r in rules:
        for sel in (s.strip() for s in r["selector"].split(",")):
            for prop, val in r["decls"]:
                out[(r["media"], sel, prop, norm_value(val))] += 1
    return out


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    a, b = multiset(argv[1]), multiset(argv[2])
    only_a, only_b = a - b, b - a
    for k, c in sorted(only_a.items()):
        print(f"only in {argv[1]} (x{c}): {k}")
    for k, c in sorted(only_b.items()):
        print(f"only in {argv[2]} (x{c}): {k}")
    print(f"{sum(a.values())} vs {sum(b.values())} declarations, "
          f"{sum(only_a.values()) + sum(only_b.values())} differences")
    return 1 if only_a or only_b else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
