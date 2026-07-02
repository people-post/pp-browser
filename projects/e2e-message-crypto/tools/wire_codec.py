#!/usr/bin/env python3
"""Shared pp Binary Wire Profile helpers (LenUtf8, LenBytes)."""

from __future__ import annotations

import struct


def len_utf8(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack(">Q", len(encoded)) + encoded


def len_bytes(data: bytes) -> bytes:
    return struct.pack(">Q", len(data)) + data


def read_len_utf8(data: bytes, offset: int = 0) -> tuple[str, int]:
    (size,) = struct.unpack_from(">Q", data, offset)
    start = offset + 8
    end = start + size
    return data[start:end].decode("utf-8"), end
