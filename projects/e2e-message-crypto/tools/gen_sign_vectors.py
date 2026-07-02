#!/usr/bin/env python3
"""Generate frozen E014 Ed25519 signing test vectors for DESIGN.md.

Binary Wire Profile: LenUtf8 for sign-string fields (D088).
"""

import base64
import hashlib
import struct

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from chatpayload_codec import VECTOR_A, VECTOR_B, VECTOR_C
from gen_aead_vectors import build_aad, e2e_blob_from_plaintext
from wire_codec import len_utf8

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
    out += len_utf8(message_id)
    out += len_utf8(sender_contact_id)
    return bytes(out)


def main() -> None:
    canonical_payload = VECTOR_A
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
    e2e_blob = e2e_blob_from_plaintext(canonical_payload, e2e_aad)
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

    test_priv = bytes.fromhex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    )
    test_pub = bytes.fromhex(
        "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8"
    )
    key = Ed25519PrivateKey.from_private_bytes(test_priv)
    derived_pub = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    assert derived_pub == test_pub

    print("vector_a_hex:", VECTOR_A.hex())
    print("vector_a_body_hash:", blake2b_256(bytes([0x01]) + VECTOR_A).hex())
    print("vector_b_hex:", VECTOR_B.hex())
    print("vector_b_body_hash:", blake2b_256(bytes([0x01]) + VECTOR_B).hex())
    print("vector_c_hex:", VECTOR_C.hex())
    print("vector_c_body_hash:", blake2b_256(bytes([0x01]) + VECTOR_C).hex())
    print("content_b64:", base64.b64encode(canonical_payload).decode())
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
