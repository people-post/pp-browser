#!/usr/bin/env python3
"""Lint: no hard-coded colours outside the generated theme files.

Colours belong in assets/themes/theme_tokens.tsv; colors-light.rcss and
colors-dark.rcss are generated (scripts/ui/gen_theme_colors.py). This check
fails when a colour literal appears anywhere in scope except those files.

Colour literal: #rgb  #rgba  #rrggbb  #rrggbbaa  (hex, any case), rgb(...), rgba(...).
(Named colours such as "white" / "transparent" are not matched.)

Scope
  * assets/themes/*.rcss except colors-light.rcss / colors-dark.rcss  (whole file
    except /* ... */ comments)
  * assets/views/*.rml and assets/icons/*.svg  (whole file except <!-- ... --> comments,
    and /* ... */ comments inside a <style> element)
    Comments are prose: "TODO #205" or "#166:" names an issue, not a colour.
  * C++ (*.cpp, *.h) under src/ and capabilities/*/src/, skipping any directory
    named tests or test. Only colours inside a string literal are considered, and
    only when that literal also looks like markup/style, i.e. it contains (case-
    insensitive) one of: style=  color  fill  stroke  background.
    String literal = a double-quoted "..." (with backslash escapes) on one line, or a
    raw string R"delim( ... )delim" which may span lines (then the whole raw string is
    the unit). Adjacent literals joined by the compiler are checked separately.
    Hex digits of length 3/4/6/8 only, delimited by a word boundary, so #include or
    "#12345" style non-colours are mostly ignored.

Self-test: check_theme_colors.py --self-test (a colour in a comment is ignored, a real
one is still reported; run by CI's lint job).

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
CSS_COMMENT_RE = re.compile(r"/\*.*?(?:\*/|\Z)", re.S)
MARKUP_COMMENT_RE = re.compile(r"<!--.*?(?:-->|\Z)", re.S)
STYLE_ELEMENT_RE = re.compile(r"(<style\b[^>]*>)(.*?)(</style\s*>|\Z)", re.S | re.I)


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


def without_comments(text, suffix):
    """The text a renderer reads: RCSS without /* */ comments; RML / SVG without <!-- --> comments and without
    /* */ comments inside <style>. An unterminated comment runs to the end, as it does for the parser."""
    if suffix == ".rcss":
        return CSS_COMMENT_RE.sub(" ", text)
    text = MARKUP_COMMENT_RE.sub(" ", text)
    return STYLE_ELEMENT_RE.sub(lambda m: m.group(1) + CSS_COMMENT_RE.sub(" ", m.group(2)) + m.group(3), text)


def chunks_of(text, suffix):
    """The parts of a file whose colour literals count."""
    if suffix in (".cpp", ".h"):
        chunks = (m.group(2) if m.group(2) is not None else m.group(3) for m in STRING_RE.finditer(text))
        return [s for s in chunks if CONTEXT_RE.search(s)]
    return [without_comments(text, suffix)]


def literals(text, suffix):
    """The colour literals of one file, rgb()/rgba() with whitespace dropped."""
    out = []
    for s in chunks_of(text, suffix):
        for m in COLOR_RE.finditer(s):
            out.append(re.sub(r"\s+", "", m.group(0)) if m.group(0).startswith("rgb") else m.group(0))
    return out


def found():
    c = Counter()
    for p, _ in files():
        for lit in literals(p.read_text(encoding="utf-8", errors="replace"), p.suffix):
            c[(rel(p), lit)] += 1
    return c


def self_test():
    cases = [
        # (suffix, text, literals wanted)
        (".rml", "<div/><!-- No microphone yet (TODO #205) --><p>x</p>", []),
        (".rml", "<!--\n multi-line #202, fill #fff\n-->", []),
        (".rml", '<div style="color: #205;"><!-- #202 --></div>', ["#205"]),
        (".rml", "<style>/* #166 */ p { color: #abc; }</style>", ["#abc"]),
        (".rml", "<p>a /* b #123 */ c</p>", ["#123"]),  # outside <style> it is text, not a comment
        (".svg", '<!-- #fff --><path fill="#000"/>', ["#000"]),
        (".rcss", "/* The reconnect capsule (#171) */\n.a { color: #0a84ff; }", ["#0a84ff"]),
        (".rcss", "/* rgba(0, 0, 0, 0.5) */ .a { background-color: rgba(0, 0, 0, 115); }", ["rgba(0,0,0,115)"]),
        (".rcss", ".a { color: #111; } /* unterminated #222", ["#111"]),
        (".cpp", 'const char* k = "style=\\"color: #123456\\""; // #abcdef', ["#123456"]),
    ]
    failures = 0
    for suffix, text, want in cases:
        got = literals(text, suffix)
        if got != want:
            print(f"self-test: {suffix} {text!r}: found {got}, wanted {want}", file=sys.stderr)
            failures += 1
    if failures == 0:
        print("check_theme_colors self-test: ok")
    return 1 if failures else 0


def load_exempt():
    ex = Counter()
    if EXEMPT_FILE.exists():
        for line in EXEMPT_FILE.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                path, lit, n = line.split("\t")
                ex[(path, lit)] = int(n)
    return ex


def rules_outside_theme_block():
    """A rule after the template's closing `}` of `@media (theme: ...)` lands in both generated files unconditionally;
    the dark file loads last, so its colours win in light mode too (2026-11-03: the phone Home page was dark in light
    mode). Returns (file, line) of every top-level rule outside the media block."""
    out = []
    for name in ("colors-light.rcss", "colors-dark.rcss"):
        path = ROOT / "assets" / "themes" / name
        depth = 0
        for no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if depth == 0 and "{" in line and not line.lstrip().startswith("@media"):
                out.append((name, no))
            depth += line.count("{") - line.count("}")
    return out


def main():
    if "--self-test" in sys.argv:
        return self_test()
    stray = rules_outside_theme_block()
    if stray:
        for name, no in stray:
            print(f"error: assets/themes/{name}:{no}: rule outside the @media (theme: ...) block; "
                  "move it above the template's last closing brace", file=sys.stderr)
        return 1
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
