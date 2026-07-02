#!/usr/bin/env python3
"""Generate frozen E014 Ed25519 signing test vectors for DESIGN.md.

Canonical sender_contact_id fixture: relay:alice123 (D082 / E017).
E2E blob uses the real AEAD fixture from gen_aead_vectors.py.
"""

import base64
import hashlib
import struct

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from gen_aead_vectors import NONCE, SESSION_KEY, build_aad, encrypt

DOMAIN = b"pp-browser:relay-envelope-sign-v1\x00"


def blake2b_256(data: bytes) -> bytes:
    return hashlib.blake2b(data, digest_size=32).digest()


def build_sign_bytes(
    body_hash: bytes,
    timestamp: int,
    sender_seq: int,
    session_epoch: int,
    channel: int,
    message_id: str,
    sender_contact_id: str,
) -> bytes:
    out = bytearray(DOMAIN)
    out += bytes([1, 1, 0, channel])
    out += struct.pack(">q", timestamp)
    out += struct.pack(">Q", sender_seq)
    out += struct.pack(">I", session_epoch)
    out += body_hash
    for value in (message_id, sender_contact_id):
        encoded = value.encode("utf-8")
        out += struct.pack(">H", len(encoded))
        out += encoded
    return bytes(out)


def main() -> None:
    canonical_payload = b'{"schema_version":1,"content_type":"text","text":"Hello","payload":{}}'
    pub_body_hash = blake2b_256(bytes([0x01]) + canonical_payload)
    pub_sign = build_sign_bytes(
        pub_body_hash,
        1719662400123,
        0,
        0,
        0,
        "550e8400-e29b-41d4-a716-446655440000",
        "relay:alice123",
    )

    e2e_aad = build_aad(
        1,
        "relay:bob456",
        "660e8400-e29b-41d4-a716-446655440001",
        "relay:alice123",
        42,
        1,
        1719662400456,
    )
    e2e_ciphertext = encrypt(SESSION_KEY, NONCE, canonical_payload, e2e_aad)
    e2e_blob = bytes([0x01]) + NONCE + e2e_ciphertext
    e2e_body_hash = blake2b_256(bytes([0x02]) + e2e_blob)
    e2e_sign = build_sign_bytes(
        e2e_body_hash,
        1719662400456,
        42,
        1,
        1,
        "660e8400-e29b-41d4-a716-446655440001",
        "relay:alice123",
    )

    # TEST ONLY — deterministic 32-byte Ed25519 private key (do not use in production).
    test_priv = bytes.fromhex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    )
    test_pub = bytes.fromhex(
        "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8"
    )
    key = Ed25519PrivateKey.from_private_bytes(test_priv)
    derived_pub = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    assert derived_pub == test_pub

    print("canonical_payload:", canonical_payload.decode())
    print("pub_body_hash:", pub_body_hash.hex())
    print("pub_sign_bytes:", pub_sign.hex())
    print("e2e_blob:", e2e_blob.hex())
    print("e2e_payload_b64:", base64.b64encode(e2e_blob).decode())
    print("e2e_body_hash:", e2e_body_hash.hex())
    print("e2e_sign_bytes:", e2e_sign.hex())
    print("test_private_key_hex:", test_priv.hex())
    print("test_public_key_hex:", test_pub.hex())
    print("test_public_key_b64:", base64.b64encode(test_pub).decode())
    print("pub_signature_b64:", base64.b64encode(key.sign(pub_sign)).decode())
    print("e2e_signature_b64:", base64.b64encode(key.sign(e2e_sign)).decode())


if __name__ == "__main__":
    main()
