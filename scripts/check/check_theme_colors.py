#!/usr/bin/env python3
"""Lint: no hard-coded colours outside the generated theme files.

Colours belong in assets/themes/theme_tokens.tsv; colors-light.rcss and
colors-dark.rcss are generated (scripts/ui/gen_theme_colors.py). This check
fails when a colour literal appears anywhere in scope except those files.

Colour literal: #rgb  #rgba  #rrggbb  #rrggbbaa  (hex, any case), rgb(...), rgba(...).
(Named colours such as "white" / "transparent" are not matched.)

Scope
  * assets/themes/*.rcss except colors-light.rcss / colors-dark.rcss  (whole file,
    comments included)
  * assets/views/*.rml and assets/icons/*.svg  (whole file)
  * C++ (*.cpp, *.h) under src/ and capabilities/*/src/, skipping any directory
    named tests or test. Only colours inside a string literal are considered, and
    only when that literal also looks like markup/style, i.e. it contains (case-
    insensitive) one of: style=  color  fill  stroke  background.
    String literal = a double-quoted "..." (with backslash escapes) on one line, or a
    raw string R"delim( ... )delim" which may span lines (then the whole raw string is
    the unit). Adjacent literals joined by the compiler are checked separately.
    Hex digits of length 3/4/6/8 only, delimited by a word boundary, so #include or
    "#12345" style non-colours are mostly ignored.

Exemptions: scripts/check/theme_color_exemptions.txt, lines "path<TAB>literal<TAB>count"
(no line numbers). The lint fails if a file has more occurrences of a literal than
exempted, or one that is not listed. Exemptions that are no longer used (or whose count
is too high) are printed but do not fail. Regenerate with --print-exemptions.

assets/icons/*.svg are white monochrome sources tinted at runtime via image-color, so
their #fff fills are expected; they are listed in the exemption file like the rest
(no special-case rule).
"""
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXEMPT_FILE = ROOT / "scripts" / "check" / "theme_color_exemptions.txt"
GENERATED = {"assets/themes/colors-light.rcss", "assets/themes/colors-dark.rcss"}
COLOR_RE = re.compile(r"(?<![0-9A-Za-z_&])#(?:[0-9a-fA-F]{8}|[0-9a-fA-F]{6}|[0-9a-fA-F]{3,4})(?![0-9A-Za-z_])"
                      r"|\brgba?\s*\([^)]*\)")
CONTEXT_RE = re.compile(r"style=|color|fill|stroke|background", re.I)
STRING_RE = re.compile(r'R"([^\s()\\]{0,16})\((.*?)\)\1"|"((?:[^"\\\n]|\\.)*)"', re.S)
SKIP_DIRS = {"tests", "test"}


def rel(p):
    return p.relative_to(ROOT).as_posix()


def files():
    for pat in ("assets/themes/*.rcss", "assets/views/*.rml", "assets/icons/*.svg"):
        for p in sorted(ROOT.glob(pat)):
            if rel(p) not in GENERATED:
                yield p, False
    roots = [ROOT / "src"] + sorted(ROOT.glob("capabilities/*/src"))
    for r in roots:
        for p in sorted(r.rglob("*")):
            if p.suffix in (".cpp", ".h") and not SKIP_DIRS & set(p.relative_to(r).parts[:-1]):
                yield p, True


def found():
    c = Counter()
    for p, cpp in files():
        text = p.read_text(encoding="utf-8", errors="replace")
        if cpp:
            chunks = (m.group(2) if m.group(2) is not None else m.group(3)
                      for m in STRING_RE.finditer(text))
            chunks = [s for s in chunks if CONTEXT_RE.search(s)]
        else:
            chunks = [text]
        for s in chunks:
            for m in COLOR_RE.finditer(s):
                c[(rel(p), re.sub(r"\s+", "", m.group(0)) if m.group(0).startswith("rgb") else m.group(0))] += 1
    return c


def load_exempt():
    ex = Counter()
    if EXEMPT_FILE.exists():
        for line in EXEMPT_FILE.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                path, lit, n = line.split("\t")
                ex[(path, lit)] = int(n)
    return ex


def main():
    c = found()
    if "--print-exemptions" in sys.argv:
        print("# path<TAB>literal<TAB>count -- existing hard-coded colours; do not add new ones.")
        for (p, lit), n in sorted(c.items()):
            print(f"{p}\t{lit}\t{n}")
        return 0
    ex = load_exempt()
    bad = [(k, n, ex.get(k, 0)) for k, n in sorted(c.items()) if n > ex.get(k, 0)]
    for k, have in sorted(ex.items()):
        if c.get(k, 0) < have:
            print(f"note: exemption not fully used: {k[0]}  {k[1]}  (listed {have}, found {c.get(k, 0)})")
    if bad:
        for (p, lit), n, allowed in bad:
            print(f"error: {p}: hard-coded colour {lit} x{n} (exempted {allowed}); "
                  "add a token in assets/themes/theme_tokens.tsv instead", file=sys.stderr)
        return 1
    print(f"ok: {sum(c.values())} exempted colour literals, none new")
    return 0


if __name__ == "__main__":
    sys.exit(main())
