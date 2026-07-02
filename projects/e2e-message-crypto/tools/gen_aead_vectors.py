#!/usr/bin/env python3
"""Generate frozen AEAD / blob codec test vectors for DESIGN.md.

Uses libsodium-compatible XChaCha20-Poly1305 via PyNaCl.
Regenerate: projects/e2e-message-crypto/tools/.venv/bin/python gen_aead_vectors.py
"""

import base64
import struct

from nacl.bindings import (
    crypto_aead_xchacha20poly1305_ietf_decrypt,
    crypto_aead_xchacha20poly1305_ietf_encrypt,
)

CANONICAL_PAYLOAD = (
    b'{"schema_version":1,"content_type":"text","text":"Hello","payload":{}}'
)

# HKDF vector (E015) — session_key for channel=e2e, session_epoch=1
SESSION_KEY = bytes.fromhex(
    "f7dab69eb0c862df230bc383c1dea363637a6caf2d46d7b57d1b45b5526a7358"
)

# Fixed nonce for reproducible fixture (TEST ONLY — production uses randombytes_buf)
NONCE = bytes(range(24))


def build_aad(
    channel: int,
    peer_contact_id: str,
    message_id: str,
    sender_contact_id: str,
    sender_seq: int,
    session_epoch: int,
    timestamp: int,
) -> bytes:
    out = bytearray([1, channel])
    for value in (peer_contact_id, message_id, sender_contact_id):
        encoded = value.encode("utf-8")
        out += struct.pack(">H", len(encoded))
        out += encoded
    out += struct.pack(">Q", sender_seq)
    out += struct.pack(">I", session_epoch)
    out += struct.pack(">q", timestamp)
    return bytes(out)


def encrypt(session_key: bytes, nonce: bytes, plaintext: bytes, aad: bytes) -> bytes:
    return crypto_aead_xchacha20poly1305_ietf_encrypt(
        plaintext, aad, nonce, session_key
    )


def decrypt(session_key: bytes, nonce: bytes, ciphertext: bytes, aad: bytes) -> bytes:
    return crypto_aead_xchacha20poly1305_ietf_decrypt(
        ciphertext, aad, nonce, session_key
    )


def main() -> None:
    # Align envelope fields with Ed25519 vector 2 (DESIGN.md)
    message_id = "660e8400-e29b-41d4-a716-446655440001"
    sender_contact_id = "relay:alice123"
    peer_contact_id = "relay:bob456"
    sender_seq = 42
    session_epoch = 1
    timestamp = 1719662400456
    channel = 1  # e2e

    aad_alice = build_aad(
        channel,
        peer_contact_id,
        message_id,
        sender_contact_id,
        sender_seq,
        session_epoch,
        timestamp,
    )

    ciphertext = encrypt(SESSION_KEY, NONCE, CANONICAL_PAYLOAD, aad_alice)
    blob = bytes([1]) + NONCE + ciphertext
    blob_b64 = base64.b64encode(blob).decode()

    # Bob decrypts with the same AAD bytes (receiver checks field semantics first)
    aad_bob = build_aad(
        channel,
        peer_contact_id,
        message_id,
        sender_contact_id,
        sender_seq,
        session_epoch,
        timestamp,
    )
    assert aad_alice == aad_bob
    recovered = decrypt(SESSION_KEY, NONCE, ciphertext, aad_bob)
    assert recovered == CANONICAL_PAYLOAD

    # Round-trip base64 codec
    assert base64.b64decode(blob_b64) == blob

    print("plaintext_utf8:", CANONICAL_PAYLOAD.decode())
    print("session_key_hex:", SESSION_KEY.hex())
    print("nonce_hex:", NONCE.hex())
    print("aad_hex:", aad_alice.hex())
    print("aad_len:", len(aad_alice))
    print("ciphertext_tag_hex:", ciphertext.hex())
    print("ciphertext_tag_len:", len(ciphertext))
    print("blob_hex:", blob.hex())
    print("blob_len:", len(blob))
    print("blob_b64:", blob_b64)
    print("peer_contact_id:", peer_contact_id)
    print("sender_contact_id:", sender_contact_id)


if __name__ == "__main__":
    main()
