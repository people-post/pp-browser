#!/usr/bin/env python3
"""Add explicit #include \"common/PbrCompat.h\" to TUs that still need pbr aliases."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "src"
SKIP_FILES = {"PbrCompat.h", "ValueJson.h"}
SUFFIXES = {".cpp", ".h", ".hpp", ".mm"}

NEED_RE = re.compile(
    r"\b("
    r"Roe|ResultOrError|Module|WorkerPool|SequencedTaskRunner|"
    r"WireLenUtf8|WireLenBytes|OutputArchive|InputArchive|binaryPack|binaryUnpack|"
    r"Object|Value|Array|Null|ObjectPtr|ArrayPtr|"
    r"asArray|asObject|asString|makeArray|valueEqual|"
    r"isArrayValue|isObjectValue|isStringValue|isBoolValue|isNullValue"
    r")\b"
)
NS_ALIAS_RE = re.compile(r"\b(logging|util|civil_time|json_io)::")


def needs_pbr_compat(text: str) -> bool:
    if "namespace pbr" not in text:
        return False
    if NEED_RE.search(text) or NS_ALIAS_RE.search(text):
        return True
    if re.search(r"namespace pbr[\s\S]{0,8000}\bError\b", text):
        return True
    return False


def already_has_include(text: str) -> bool:
    return "common/PbrCompat.h" in text


def _is_include(stripped: str) -> bool:
    return stripped.startswith("#include") or stripped.startswith("#import")


def first_include_end(lines: list[str]) -> int:
    """Index after the first contiguous #include/#import block near the top of the file."""
    insert_at = 0
    in_leading_if = True
    for i, line in enumerate(lines):
        stripped = line.strip()
        if in_leading_if and (
            stripped.startswith("#if")
            or stripped.startswith("#ifdef")
            or stripped.startswith("#ifndef")
            or stripped.startswith("#elif")
            or stripped == ""
        ):
            if _is_include(stripped):
                in_leading_if = False
                insert_at = i + 1
            continue
        if _is_include(stripped):
            insert_at = i + 1
            in_leading_if = False
        elif insert_at > 0 and stripped and not stripped.startswith("#"):
            break
        elif insert_at > 0 and stripped.startswith("#") and not _is_include(stripped):
            # Stop before #pragma/#endif/etc. so we never insert mid-function
            # (e.g. after #pragma clang diagnostic inside an ObjC method).
            break
    return insert_at


def insert_include(text: str) -> str:
    lines = text.splitlines(keepends=True)
    insert_at = first_include_end(lines)
    include_line = '#include "common/PbrCompat.h"\n'
    if insert_at == 0:
        return include_line + text
    lines.insert(insert_at, include_line)
    return "".join(lines)


def main() -> None:
    changed = 0
    for path in sorted(ROOT.rglob("*")):
        if path.suffix not in SUFFIXES or path.name in SKIP_FILES:
            continue
        text = path.read_text(encoding="utf-8")
        if already_has_include(text) or not needs_pbr_compat(text):
            continue
        path.write_text(insert_include(text), encoding="utf-8")
        changed += 1
        print(path.relative_to(ROOT.parent))
    print(f"Updated {changed} files")


if __name__ == "__main__":
    main()
