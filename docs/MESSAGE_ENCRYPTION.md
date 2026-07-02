# Message encryption (E2E)

Normative spec for symmetric end-to-end encryption of direct chat message bodies on the `e2e` channel. Planning, phases, and ADRs live in [projects/e2e-message-crypto/](../projects/e2e-message-crypto/).

**Related:** [P2P_MESSAGING.md](P2P_MESSAGING.md), [chat-storage WIRE_SCHEMAS.md](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md), [CONFIGURATION.md](CONFIGURATION.md).

## Overview

- **256-bit pre-shared key (PSK)** per `ChatTargetKey`, distributed out-of-band with fingerprint verification.
- **HKDF-SHA256** derives per-epoch session keys from the PSK.
- **XChaCha20-Poly1305** (libsodium AEAD) encrypts the message body with canonical associated data (AAD).
- **Ed25519** signs the outer relay envelope (classical; PQ hybrid planned separately).
- Relay sees ciphertext on `e2e`; plaintext `ChatPayload` JSON is inside the AEAD layer only.

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
- Initial setup: paste raw base64 PSK. Rotation: `pp-browser-psk-bundle-v1` JSON (see project DESIGN).

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

`aad_version = 1` only. Big-endian integers.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `aad_version` = `1` |
| 1 | 1 | `channel`: `0` = public_relay, `1` = e2e |
| | 2 | `peer_contact_id_len` (u16 BE) |
| | var | `peer_contact_id` UTF-8 |
| | 2 | `message_id_len` (u16 BE) |
| | var | `message_id` UTF-8 |
| | 2 | `sender_contact_id_len` (u16 BE) |
| | var | `sender_contact_id` UTF-8 |
| | 8 | `sender_seq` (u64 BE) |
| | 4 | `session_epoch` (u32 BE) |
| | 8 | `timestamp` (i64 BE) |

**Sender** sets `peer_contact_id` to recipient identity, `sender_contact_id` to own identity.

**Receiver** verifies before decrypt: `peer_contact_id` = local self identity; `sender_contact_id` = `envelope.sender_contact_id`; remaining fields match signed envelope. Wrong AAD → hard fail.

## AEAD plaintext (E010)

UTF-8 JSON **`ChatPayload`** (minified for signing/hashing where applicable):

```json
{"schema_version":1,"content_type":"text","text":"Hello","payload":{}}
```

Max decrypted size: **128 KiB** (`kMaxE2ePlaintextBytes`). Check length before JSON parse.

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
| `public_relay` | `{ "content": { …ChatPayload… } }` |
| `e2e` | `{ "e2e": { "payload_b64": "…" } }` |

Reject envelopes with `thread_id` or unknown `envelope_version`.

## Ed25519 canonical signing bytes (E014)

Do **not** sign JSON dump. Build fixed binary bytes, then Ed25519-sign.

**Domain prefix** (34 bytes): `"pp-browser:relay-envelope-sign-v1"` + NUL.

Then: `sign_version=1`, `envelope_version=1`, `route_kind`, `channel`, `timestamp` (i64 BE), `sender_seq` (u64 BE), `session_epoch` (u32 BE), `body_hash` (32 bytes BLAKE2b-256), length-prefixed `message_id`, length-prefixed `sender_contact_id`.

**Body hash:**

```
body_hash = BLAKE2b-256( body_kind || payload_bytes )
```

| Channel | `body_kind` | `payload_bytes` |
|---------|-------------|-----------------|
| `public_relay` | `0x01` | Canonical UTF-8 JSON of `body.content` |
| `e2e` | `0x02` | Raw bytes from base64 decode of `body.e2e.payload_b64` |

`signature` on wire: base64 over 64-byte Ed25519 signature.

Peer verify keys: relay directory + `PeerSigningKeyStore` (E016). Inbound verify before decrypt.

## Send / receive pipeline (e2e)

1. Build `ChatPayload` JSON → canonical AAD from envelope fields → `MessageCipher::Encrypt` → blob → base64 → `body.e2e.payload_b64`.
2. `EnvelopeSigner::BuildSignBytes` → sign.
3. Receive: verify signature → resolve PSK for `envelope.session_epoch` → decrypt with AAD from envelope → parse JSON → ingest.

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

Regenerate: `projects/e2e-message-crypto/tools/gen_sign_vectors.py`, `gen_aead_vectors.py`.

### Shared Ed25519 test keypair (TEST ONLY)

| Field | Value |
|-------|-------|
| Private key (hex) | `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f` |
| Public key (hex) | `03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8` |
| Public key (base64) | `A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=` |

### Ed25519 — `public_relay`

Canonical payload: `{"schema_version":1,"content_type":"text","text":"Hello","payload":{}}`

| Field | Value |
|-------|-------|
| `message_id` | `550e8400-e29b-41d4-a716-446655440000` |
| `sender_contact_id` | `relay:alice123` |
| `timestamp` | `1719662400123` |
| **`body_hash` (hex)** | `db8f17cda6b57a0feff3b6aa09ca17e7ca15b32309cc85d555531c804e2c7f10` |
| **`sign_bytes` (hex)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100000000019063ddd27b000000000000000000000000db8f17cda6b57a0feff3b6aa09ca17e7ca15b32309cc85d555531c804e2c7f10002435353065383430302d653239622d343164342d613731362d343436363535343430303030000e72656c61793a616c696365313233` |
| **`signature` (base64)** | `sgoePjY8ExAV+yVono5XyO6UUosHP0ka4Ham8f/2sKlUQwJvzbq1VFX+DWJlDVGZArw1MyPzQp44/H5+2zwGCA==` |

### Ed25519 — `e2e`

| Field | Value |
|-------|-------|
| `message_id` | `660e8400-e29b-41d4-a716-446655440001` |
| `sender_contact_id` | `relay:alice123` |
| `timestamp` | `1719662400456` |
| `sender_seq` | `42` |
| `session_epoch` | `1` |
| **`payload_b64`** | `AQABAgMEBQYHCAkKCwwNDg8QERITFBUWFyHtOqqqWWg/pqrknkBhKHaF+SXAyEkn1r2loSMVWmnO1HgQ7B/uYrcLE5SZn7v/8/ZvnGJsuW+StHBZWIFUKGrC5cggZHoHelCS/RLpokmfv/AOd1cv` |
| **`body_hash` (hex)** | `845179587525a14c1dd5a19099fbff4a47d06f7c458967ab4969fedaa748bbbe` |
| **`sign_bytes` (hex)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100010000019063ddd3c8000000000000002a00000001845179587525a14c1dd5a19099fbff4a47d06f7c458967ab4969fedaa748bbbe002436363065383430302d653239622d343164342d613731362d343436363535343430303031000e72656c61793a616c696365313233` |
| **`signature` (base64)** | `5+RuBH37ArdNTcd5U+dMy7xu2nJxRM3ruNMU75vBVi3aN1ftgmO2fXsb94NT5IaQaYW6wfxRWU4IVvPRxtDhDg==` |

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
| **`aad` (hex)** | `0101000c72656c61793a626f62343536002436363065383430302d653239622d343164342d613731362d343436363535343430303031000e72656c61793a616c696365313233000000000000002a000000010000019063ddd3c8` |
| `plaintext` | `{"schema_version":1,"content_type":"text","text":"Hello","payload":{}}` |
| **`ciphertext+tag` (hex)** | `21ed3aaaaa59683fa6aae49e4061287685f925c0c84927d6bda5a123155a69ced47810ec1fee62b70b1394999fbbfff3f66f9c626cb96f92b47059588154286ac2e5c820647a077a5092fd12e9a2499fbff00e77572f` |
| **`blob` (hex)** | `01000102030405060708090a0b0c0d0e0f101112131415161721ed3aaaaa59683fa6aae49e4061287685f925c0c84927d6bda5a123155a69ced47810ec1fee62b70b1394999fbbfff3f66f9c626cb96f92b47059588154286ac2e5c820647a077a5092fd12e9a2499fbff00e77572f` |
| **`payload_b64`** | `AQABAgMEBQYHCAkKCwwNDg8QERITFBUWFyHtOqqqWWg/pqrknkBhKHaF+SXAyEkn1r2loSMVWmnO1HgQ7B/uYrcLE5SZn7v/8/ZvnGJsuW+StHBZWIFUKGrC5cggZHoHelCS/RLpokmfv/AOd1cv` |

Bob decrypts with the same AAD bytes after field semantic checks.
