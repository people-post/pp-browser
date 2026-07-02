# Message encryption (E2E)

Normative spec for symmetric end-to-end encryption of direct chat message bodies on the `e2e` channel. Planning, phases, and ADRs live in [projects/e2e-message-crypto/](../projects/e2e-message-crypto/).

**Related:** [P2P_MESSAGING.md](P2P_MESSAGING.md), [chat-storage WIRE_SCHEMAS.md](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md), [CONFIGURATION.md](CONFIGURATION.md).

**C++ wire profile:** [Serialize.hpp](../src/common/Serialize.hpp) (`WireLenUtf8`, `WireLenBytes`); [Binary Wire Profile (D088)](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md#pp-binary-wire-profile-d088).

## Overview

- **256-bit pre-shared key (PSK)** per `ChatTargetKey`, distributed out-of-band with fingerprint verification.
- **HKDF-SHA256** derives per-epoch session keys from the PSK.
- **XChaCha20-Poly1305** (libsodium AEAD) encrypts the message body with canonical associated data (AAD).
- **Ed25519** signs the outer relay envelope (classical; PQ hybrid planned separately).
- Relay sees ciphertext on `e2e`; plaintext **binary `ChatPayload`** (D087) is inside the AEAD layer only.

## Threat model (v1)

| Adversary capability | Protected by |
|----------------------|--------------|
| Relay reads E2E body | AEAD ciphertext |
| Relay forges E2E ciphertext without PSK | AEAD + seq in AAD |
| Relay forges envelope (wrong sender) | Ed25519 verify + pinned peer signing key |
| Network replay of captured E2E blob | `sender_seq` in AAD + ingest rules |

Not protected in v1: traffic metadata, classical Ed25519 break, local disk theft of PSK JSON.

## Algorithms

| Layer | Algorithm | Library |
|-------|-----------|---------|
| Message body | XChaCha20-Poly1305 | libsodium |
| Key derivation | HKDF-SHA256 | libsodium |
| PSK fingerprint | BLAKE2b-256 | libsodium |
| Master PSK | 32 random bytes | libsodium `randombytes_buf` |
| Relay envelope sig | Ed25519 | OpenSSL EVP (existing) |

## Key material

### Master PSK

- 32 bytes from CSPRNG; fingerprint = BLAKE2b-256(PSK) as grouped hex.
- Initial setup (E011): either peer generates; export raw base64 + fingerprint OOB; peer imports; both confirm fingerprint before first send (`psk_verified_at` on `chat_targets`). Rotation: export/import `pp-browser-psk-bundle-v1` JSON (see project DESIGN).

### Session key (HKDF — E015)

```
session_key = HKDF-SHA256(
  ikm   = master_psk,
  salt  = "pp-browser-msg-v1",
  info  = "channel:{channel}|epoch:{session_epoch}"
)
```

- `channel`: `e2e` for body encryption; `public_relay` has no PSK session.
- `session_epoch`: uint32; bumped on rotation. Both peers derive the same key from shared PSK + `(channel, epoch)` — identity strings are **not** in HKDF `info`.
- PSK columns live on `profile.db` → `chat_targets` (same row as seq/epoch).

### Identity in AAD and wire (D079)

- **`sender_contact_id`** on wire = sender's **communicating identity value** (e.g. `relay:abc123`).
- **`peer_contact_id` in AAD** = recipient's communicating identity value (from sender's view).
- **`Contact.id`** and **`local:self`** are never in AAD or relay envelope.
- **`thread_id`** is never in AAD or relay envelope.

## AEAD associated data (AAD)

`aad_version = 1` only. Big-endian integers. Identity and id strings use **LenUtf8** (D088).

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `aad_version` = `1` |
| 1 | 1 | `channel`: `0` = public_relay, `1` = e2e |
| | var | `peer_contact_id` — **LenUtf8** |
| | var | `message_id` — **LenUtf8** |
| | var | `sender_contact_id` — **LenUtf8** |
| | 8 | `sender_seq` (u64 BE) |
| | 4 | `session_epoch` (u32 BE) |
| | 8 | `timestamp` (i64 BE) |

**Sender** sets `peer_contact_id` to recipient identity, `sender_contact_id` to own identity.

**Receiver** verifies before decrypt: `peer_contact_id` = local self identity; `sender_contact_id` = `envelope.sender_contact_id`; remaining fields match signed envelope. Wrong AAD → hard fail.

## AEAD plaintext (E010)

Binary **`ChatPayload` v1** — same bytes as public **`body.content_b64`** after base64 decode. Layout: [WIRE_SCHEMAS § ChatPayload](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md#chatpayload-v1--binary-d087d088).

**Vector A fixture** (`text="Hello"`, plain default):

| Field | Value |
|-------|-------|
| **`bytes` (hex)** | `0100000000000000000548656c6c6f` |
| **`content_b64`** | `AQAAAAAAAAAABUhlbGxv` |

Max decrypted size: **128 KiB** (`kMaxE2ePlaintextBytes`). Check length before `ChatPayloadCodec::Decode`; reject trailing bytes.

## Encrypted payload blob

Binary inside `body.e2e.payload_b64` (RFC 4648 base64):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `payload_version` = `1` |
| 1 | 24 | `nonce` (random in production) |
| 25 | var | ciphertext + Poly1305 tag |

API: `crypto_aead_xchacha20poly1305_ietf_encrypt` / `_decrypt` with `npub` = nonce, `ad` = AAD, `k` = session_key.

## Relay envelope (e2e)

```json
{
  "envelope_version": 1,
  "message_id": "uuid",
  "sender_relay_id": "relay:alice123",
  "sender_contact_id": "relay:alice123",
  "route": { "kind": "direct", "channel": "e2e" },
  "sender_seq": 42,
  "session_epoch": 1,
  "body": { "e2e": { "payload_b64": "…" } },
  "timestamp": 1719662400123,
  "signature": "…"
}
```

| Channel | `body` shape |
|---------|--------------|
| `public_relay` | `{ "content_b64": "…" }` |
| `e2e` | `{ "e2e": { "payload_b64": "…" } }` |

Reject envelopes with `thread_id` or unknown `envelope_version`.

## Ed25519 canonical signing bytes (E014)

Do **not** sign JSON dump. Build fixed binary bytes, then Ed25519-sign. String fields use **LenUtf8** (D088).

**Domain prefix** (34 bytes): `"pp-browser:relay-envelope-sign-v1"` + NUL.

Then: `sign_version=1`, `envelope_version=1`, `route_kind`, `channel`, `timestamp` (i64 BE), `sender_seq` (u64 BE), `session_epoch` (u32 BE), `body_hash` (32 bytes BLAKE2b-256), `message_id` LenUtf8, `sender_contact_id` LenUtf8.

**Body hash:**

```
body_hash = BLAKE2b-256( body_kind || payload_bytes )
```

| Channel | `body_kind` | `payload_bytes` |
|---------|-------------|-----------------|
| `public_relay` | `0x01` | Raw bytes from base64 decode of `body.content_b64` |
| `e2e` | `0x02` | Raw bytes from base64 decode of `body.e2e.payload_b64` |

`signature` on wire: base64 over 64-byte Ed25519 signature.

Peer verify keys: relay directory + `PeerSigningKeyStore` (E016). Inbound verify before decrypt.

## Send / receive pipeline (e2e)

1. Build binary `ChatPayload` → canonical AAD from envelope fields → `MessageCipher::Encrypt` → blob → base64 → `body.e2e.payload_b64`.
2. `EnvelopeSigner::BuildSignBytes` → sign.
3. Receive: verify signature → resolve PSK for `envelope.session_epoch` → decrypt with AAD from envelope → `ChatPayloadCodec::Decode` → ingest.

Outbound HKDF uses `chat_targets.session_epoch`; inbound uses `envelope.session_epoch` (E019).

## `base/crypto` module (target)

| Component | Role |
|-----------|------|
| `SessionKeyDeriver` | HKDF-SHA256 |
| `CanonicalAad` | Build/parse AAD |
| `MessageCipher` | AEAD encrypt/decrypt |
| `EncryptedPayload` | Blob codec + base64 |
| `ReplayWindow` | Seq acceptance helper (D013 classifier is authoritative) |
| `IPskSessionStore` | PSK + retired epochs on `chat_targets` |

Implementation: [projects/e2e-message-crypto/PHASES.md](../projects/e2e-message-crypto/PHASES.md) phase c1.

## Frozen test vectors

Regenerate: `projects/e2e-message-crypto/tools/gen_sign_vectors.py`, `gen_aead_vectors.py`, `chatpayload_codec.py`.

### Shared Ed25519 test keypair (TEST ONLY)

| Field | Value |
|-------|-------|
| Private key (hex) | `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f` |
| Public key (hex) | `03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8` |
| Public key (base64) | `A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=` |

### Ed25519 — `public_relay`

Binary payload Vector A: `0100000000000000000548656c6c6f` (`content_b64`: `AQAAAAAAAAAABUhlbGxv`)

| Field | Value |
|-------|-------|
| `message_id` | `550e8400-e29b-41d4-a716-446655440000` |
| `sender_contact_id` | `relay:alice123` |
| `timestamp` | `1719662400123` |
| **`body_hash` (hex)** | `c3883ac60f3d527e364ecca8dd28144886dc12f00fbef22502ef0f24ce2f1c74` |
| **`sign_bytes` (hex)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100000000019063ddd27b000000000000000000000000c3883ac60f3d527e364ecca8dd28144886dc12f00fbef22502ef0f24ce2f1c74000000000000002435353065383430302d653239622d343164342d613731362d343436363535343430303030000000000000000e72656c61793a616c696365313233` |
| **`signature` (base64)** | `Gc6/4LugFNm7SKtGGicoUnUp9bWqJa6f71jYGSp+yUYn2UjdbkXwYpAz7fcSwrrwVSdrnxU98SveSAijpWsFDQ==` |

### Ed25519 — `e2e`

| Field | Value |
|-------|-------|
| `message_id` | `660e8400-e29b-41d4-a716-446655440001` |
| `sender_contact_id` | `relay:alice123` |
| `timestamp` | `1719662400456` |
| `sender_seq` | `42` |
| `session_epoch` | `1` |
| **`payload_b64`** | `AQABAgMEBQYHCAkKCwwNDg8QERITFBUWF1vPScnCPAVe+dnJiV9kKBztMM3qj/Hi+RhfLy6wlhU=` |
| **`body_hash` (hex)** | `b09daad4a14b17961c834c3b027c3d03ef49a0b1f3bffaa7c8c22da097a8042e` |
| **`sign_bytes` (hex)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100010000019063ddd3c8000000000000002a00000001b09daad4a14b17961c834c3b027c3d03ef49a0b1f3bffaa7c8c22da097a8042e000000000000002436363065383430302d653239622d343164342d613731362d343436363535343430303031000000000000000e72656c61793a616c696365313233` |
| **`signature` (base64)** | `QeHtXYUc4uQJ+1qCSYtRpabAI/kk7Mik04kqQVOKk+O7WWO64VPnnNUUeaTmEX3BSOconoKo1ZwFVNO+JoGACA==` |

### HKDF (E015)

| Field | Value |
|-------|-------|
| `master_psk` (hex) | `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f` |
| `salt` | `pp-browser-msg-v1` |
| `info` | `channel:e2e\|epoch:1` |
| **`session_key` (hex)** | `f7dab69eb0c862df230bc383c1dea363637a6caf2d46d7b57d1b45b5526a7358` |

### AEAD / codec

Alice (`relay:alice123`) → Bob (`relay:bob456`). Envelope fields match Ed25519 e2e vector.

| Field | Value |
|-------|-------|
| `session_key` (hex) | `f7dab69eb0c862df230bc383c1dea363637a6caf2d46d7b57d1b45b5526a7358` |
| `nonce` (hex) | `000102030405060708090a0b0c0d0e0f1011121314151617` |
| **`aad` (hex)** | `0101000000000000000c72656c61793a626f62343536000000000000002436363065383430302d653239622d343164342d613731362d343436363535343430303031000000000000000e72656c61793a616c696365313233000000000000002a000000010000019063ddd3c8` |
| `plaintext` (hex) | `0100000000000000000548656c6c6f` |
| **`ciphertext+tag` (hex)** | `5bcf49c9c23c055ef9d9c9895f64281ced30cdea8ff1e2f9185f2f2eb09615` |
| **`blob` (hex)** | `01000102030405060708090a0b0c0d0e0f10111213141516175bcf49c9c23c055ef9d9c9895f64281ced30cdea8ff1e2f9185f2f2eb09615` |
| **`payload_b64`** | `AQABAgMEBQYHCAkKCwwNDg8QERITFBUWF1vPScnCPAVe+dnJiV9kKBztMM3qj/Hi+RhfLy6wlhU=` |

Bob decrypts with the same AAD bytes after field semantic checks.

### ChatPayload — Vectors B and C (body_hash only)

| Vector | Logical | **`bytes` (hex)** | **`body_hash` (hex)** |
|--------|---------|-------------------|------------------------|
| B | `text="Café ☕"` | `01000000000000000009436166c3a920e29895` | `e02629e02b6a8328d2d69bdfc1f0fcd12646f1013684939e687d610692517eba` |
| C | `system` + LenUtf8 tail | `0101000000000000000b50656572206a6f696e656401000000000000000d6d656d6265725f6a6f696e65640000000000000005616c696365` | `b5bd1016b9ae31b1269638fe3fb86f44e1f74dfedc03bf3041812f885757453f` |
