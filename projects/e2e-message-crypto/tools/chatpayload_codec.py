#!/usr/bin/env python3
"""Binary ChatPayload v1 encoder (D087) for frozen test vectors.

Uses pp Binary Wire Profile: LenUtf8 = u64 BE + UTF-8 bytes.
Layout: projects/chat-storage-and-memory/WIRE_SCHEMAS.md
"""

from __future__ import annotations

import struct

PAYLOAD_VERSION = 1

CONTENT_TYPE_TEXT = 0
CONTENT_TYPE_SYSTEM = 1

TEXT_FORMAT_PLAIN = 0
TEXT_SUB_VERSION = 1
SYSTEM_SUB_VERSION = 1


def len_utf8(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack(">Q", len(encoded)) + encoded


def len_bytes(data: bytes) -> bytes:
    return struct.pack(">Q", len(data)) + data


def encode_chatpayload_text(*, text: str, format_plain: bool = False) -> bytes:
    """Canonical text payload. Omits sub_version/format when plain default."""
    out = bytearray([PAYLOAD_VERSION, CONTENT_TYPE_TEXT])
    out += len_utf8(text)
    if format_plain:
        out += bytes([TEXT_SUB_VERSION, TEXT_FORMAT_PLAIN])
    return bytes(out)


def encode_chatpayload_system(*, text: str, control_type: str, detail: str = "") -> bytes:
    out = bytearray([PAYLOAD_VERSION, CONTENT_TYPE_SYSTEM])
    out += len_utf8(text)
    out += bytes([SYSTEM_SUB_VERSION])
    out += len_utf8(control_type)
    out += len_utf8(detail)
    return bytes(out)


# Frozen vector A — ASCII text, plain default (no format tail).
VECTOR_A = encode_chatpayload_text(text="Hello")

# Frozen vector B — non-ASCII text, plain default (no format tail).
VECTOR_B = encode_chatpayload_text(text="Café ☕")

# Frozen vector C — system with non-empty tail (LenUtf8 fields).
VECTOR_C = encode_chatpayload_system(
    text="Peer joined",
    control_type="member_joined",
    detail="alice",
)
