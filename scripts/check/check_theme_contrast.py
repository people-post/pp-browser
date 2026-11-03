#!/usr/bin/env python3
"""Check text / background contrast of the theme tokens (assets/themes/theme_tokens.tsv).

Two tiers, both themes (decided 2026-11-03: keep the Apple palette, check by tier):
  * body text  (label)                                   >= 4.5 : 1
  * secondary  (label-2, accent / danger as text,
                white text on accent, bubble-out, danger, mention)  >= 3.0 : 1
Not checked: label-3 (placeholder, disabled) and icon-only colours (secure, warn, gray, success).

Translucent colours are composited: a background over the theme's `bg`, a foreground over its
background. Exit 1 on any pair below its tier, listing them.

Usage: python3 -I scripts/check/check_theme_contrast.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TSV = ROOT / "assets" / "themes" / "theme_tokens.tsv"

SURFACES = ["bg", "bg-grouped", "bg-card", "bg-card-2", "bg-chrome", "chat-bg", "bubble-in"]
PAIRS = (
    [("label", s, 4.5) for s in SURFACES]
    + [("label-2", s, 3.0) for s in SURFACES]
    + [("accent", s, 3.0) for s in ["bg", "bg-grouped", "bg-card", "bg-chrome", "chat-bg"]]
    + [("danger", s, 3.0) for s in ["bg", "bg-grouped", "bg-card"]]
    + [("label-on-accent", s, 3.0) for s in ["accent", "bubble-out", "danger", "mention"]]
)


def parse(value):
    v = value.strip().lstrip("#")
    if len(v) not in (6, 8):
        raise ValueError(value)
    rgb = [int(v[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    alpha = int(v[6:8], 16) / 255 if len(v) == 8 else 1.0
    return rgb, alpha


def over(fg, alpha, bg):
    return [f * alpha + b * (1 - alpha) for f, b in zip(fg, bg)]


def luminance(rgb):
    lin = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in rgb]
    return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]


def ratio(a, b):
    hi, lo = sorted((luminance(a), luminance(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


def load():
    tokens = {}
    for line in TSV.read_text(encoding="utf-8").splitlines()[1:]:
        cols = line.split("\t")
        if len(cols) >= 3:
            tokens[cols[0]] = {"light": cols[1], "dark": cols[2]}
    return tokens


def main():
    tokens = load()
    failures, missing = [], set()
    for theme in ("light", "dark"):
        base_rgb, _ = parse(tokens["bg"][theme])
        for fg_name, bg_name, need in PAIRS:
            if fg_name not in tokens or bg_name not in tokens:
                missing.update(n for n in (fg_name, bg_name) if n not in tokens)
                continue
            bg_rgb, bg_a = parse(tokens[bg_name][theme])
            surface = over(bg_rgb, bg_a, base_rgb)
            fg_rgb, fg_a = parse(tokens[fg_name][theme])
            got = ratio(over(fg_rgb, fg_a, surface), surface)
            if got + 1e-9 < need:
                failures.append(f"{theme}: {fg_name} on {bg_name} = {got:.2f} (needs {need})")
    if missing:
        print("missing tokens: " + ", ".join(sorted(missing)))
        return 1
    if failures:
        print("\n".join(failures))
        return 1
    print(f"contrast ok: {len(PAIRS)} pairs x 2 themes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
