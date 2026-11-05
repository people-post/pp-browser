#!/usr/bin/env python3
"""Fail when assets/locales/en.json and zh-Hans.json do not carry the same keys.

Usage: python3 scripts/check/check_locale_keys.py [repo_root]
"""
import json
import sys
from pathlib import Path


def flat_keys(node, prefix=""):
    keys = set()
    for key, value in node.items():
        if isinstance(value, dict):
            keys |= flat_keys(value, f"{prefix}{key}.")
        else:
            keys.add(f"{prefix}{key}")
    return keys


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    locales = root / "assets" / "locales"
    en = flat_keys(json.loads((locales / "en.json").read_text(encoding="utf-8")))
    zh = flat_keys(json.loads((locales / "zh-Hans.json").read_text(encoding="utf-8")))
    only_en, only_zh = sorted(en - zh), sorted(zh - en)
    for key in only_en:
        print(f"missing in zh-Hans.json: {key}")
    for key in only_zh:
        print(f"missing in en.json: {key}")
    if only_en or only_zh:
        return 1
    print(f"locale keys match ({len(en)} keys)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
