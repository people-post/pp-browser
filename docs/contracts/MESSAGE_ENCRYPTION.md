# Message encryption (E2E)

**Tier:** contract

Normative spec for symmetric end-to-end encryption of P2P chat message bodies. **Three tiers** (private direct, public direct, group) — all product P2P uses ciphertext on the wire ([D089](../../projects/chat-storage-and-memory/DECISIONS.md#d089--three-chat-tiers-both-direct-tiers-e2e-e021), [E021](../../projects/e2e-message-crypto/DECISIONS.md#e021--three-chat-tiers-both-direct-tiers-e2e-d089)). Planning, phases, and ADRs live in [projects/e2e-message-crypto/](../../projects/e2e-message-crypto/).

**Related:** [P2P_MESSAGING.md](../architecture/P2P_MESSAGING.md), [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md), [COMPATIBILITY.md](COMPATIBILITY.md), [DATA_LAYOUT.md](DATA_LAYOUT.md).

**C++ wire profile:** [Serialize.hpp](../../src/common/Serialize.hpp) (`WireLenUtf8`, `WireLenBytes`); [Binary Wire Profile (D088)](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088).

## Overview

- **256-bit pre-shared key (PSK)** per `ChatTargetKey` on direct tiers (`e2e`, `e2e_public`).
- **Private direct (`e2e`):** PSK distributed out-of-band with mandatory fingerprint verification (E011).
- **Public direct (`e2e_public`):** hybrid KEM auto-key (E013/E024); encapsulate to the recipient **account** ML-KEM-768 (**M015**).
- **Peer keys** (signing and KEM) come from **account cards** that prove themselves against the Account ID ([ACCOUNT_CARD.md](ACCOUNT_CARD.md), E028): the card store, the peer's own devices, the relay directory when one is configured, the chain `[later]`. No source is trusted; none is required.
- **HKDF-SHA256** derives per-epoch session keys from the PSK.
- **XChaCha20-Poly1305** (libsodium AEAD) encrypts the message body with canonical associated data (AAD).
- **ML-DSA-65** (FIPS 204) signs the outer relay envelope, with the sender's signing key (`IPeerSigningKeyResolver`).
- Relay sees ciphertext on both direct tiers; plaintext **binary `ChatPayload`** (D087) is inside the AEAD layer only.
- **No `public_relay`** — reject plaintext direct wire ([D090](../../projects/chat-storage-and-memory/DECISIONS.md#d090--no-public_relay--plaintext-direct-wire), [E023](../../projects/e2e-message-crypto/DECISIONS.md#e023--no-public_relay-wire-value-d090)).

## Threat model (v1)

| Adversary capability | Protected by |
|----------------------|--------------|
| Relay reads E2E body | AEAD ciphertext |
| Relay forges E2E ciphertext without PSK | AEAD + seq in AAD |
| Relay forges envelope (wrong sender) | ML-DSA-65 verify + pinned peer signing key |
| Relay or any carrier substitutes a peer's keys | signing key hashes to the Account ID; KEM key needs the account's binding (account cards) |
| Network replay of captured E2E blob | `sender_seq` in AAD + ingest rules |

Not protected in v1: traffic metadata. Local disk: identity, PSK, and transcript bodies/previews/memory under the PIN/DEK vault ([AT_REST_ENCRYPTION.md](AT_REST_ENCRYPTION.md), chat-storage **D102**); transcript metadata (timestamps, delivery, titles) stays plaintext.

## Algorithms

| Layer | Algorithm | Library |
|-------|-----------|---------|
| Message body | XChaCha20-Poly1305 | libsodium |
| Key derivation | HKDF-SHA256 | libsodium |
| PSK fingerprint | BLAKE2b-256 | libsodium |
| Master PSK | 32 random bytes | libsodium `randombytes_buf` |
| Relay envelope sig | ML-DSA-65 | `pp-cpp-crypto` (`mldsa-native`) |

## Key material

### Master PSK

- 32 bytes from CSPRNG; fingerprint = BLAKE2b-256(PSK) as grouped hex.
- Initial setup (E011): either peer generates; export raw base64 + fingerprint OOB; peer imports; both confirm fingerprint before first send (`psk_verified_at` on `chat_targets`). Rotation: export/import `pp-browser-psk-bundle-v1` JSON (see project DESIGN).
- **Public direct (`e2e_public`):** encapsulate to the recipient **account** ML-KEM-768 (E013 / E024 / **M015**) → `master_psk` via HKDF `info = "auto-key-v1|channel:e2e_public"`. `body.e2e.key_init_b64` on every message under a session the sender opened until the peer has been heard under it, with the sender's key card in `body.e2e.sender_keys` ([ACCOUNT_CARD.md](ACCOUNT_CARD.md#key-card)) so the peer can verify it and answer without any lookup. **Pair convergence** (`ReconcileE2ePublicSession`, after the envelope signature verifies): the receiver reads under its stored session for the epoch, else under the session the envelope's `key_init` opens (encapsulated to its own account KEM, so only the peer could have made it). Read under the stored one: a session we opened is confirmed (stop attaching `key_init`), and the envelope's signed timestamp is remembered as the latest the peer wrote under it. Read under the `key_init`: the peer is on that session and we move to it — unless both opened at once and ours, unconfirmed, has the lower session id (`AutoKeySessionId` = BLAKE2b-256 of the `key_init`), in which case the peer moves to ours; or unless the envelope was signed no later than the peer's latest write under our session (a late or replayed opener: the peer has since moved on; both timestamps are the peer's clock). On the control lane this runs after the freshness and first-sight checks. A lost first message, both sides opening at once, and one side starting over (reinstall) all converge instead of locking the pair out. A device that got its sessions back without its message numbering (account backup restore, *Reset chats*) holds them `restart_pending`: its next send in the pair moves to `session_epoch + 1` from `sender_seq` 1 under a fresh session (`key_init`), the old master kept as retired for the old epoch. A receiver reading a `key_init` at a higher epoch than its own retires its master at its epoch and moves to the new epoch and session; an envelope at a lower epoch than the stored session leaves the session alone. Linked devices share that account KEM so any of them can open account-scope `key_init`. Device-lock rekey (**E027**): system `control_type=psk_rotate`; new PSK in `key_init_b64` wrapped to account KEM or conversation KEM (never under the old PSK); `detail` binds `key_init_hash`. Account-scope chats do not auto-`rotate_psk`. Quiet auto-rekey only when both sides are device-bound. Relay must not learn `master_psk`. Private (`e2e`) does not use this handshake.

### Session key (HKDF — E015)

```
session_key = HKDF-SHA256(
  ikm   = master_psk,
  salt  = "pp-browser-msg-v1",
  info  = "channel:{channel}|epoch:{session_epoch}"
)
```

- `channel`: `e2e` (private) or `e2e_public` (public) — both encrypted (D090).
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
| 1 | 1 | `channel`: `0` = `e2e`, `1` = `e2e_public` (E023) |
| | var | `peer_contact_id` — **LenUtf8** |
| | var | `message_id` — **LenUtf8** |
| | var | `sender_contact_id` — **LenUtf8** |
| | 8 | `sender_seq` (u64 BE) |
| | 4 | `session_epoch` (u32 BE) |
| | 8 | `timestamp` (i64 BE) |

**Sender** sets `peer_contact_id` to recipient identity, `sender_contact_id` to own identity.

**Receiver** verifies before decrypt: `peer_contact_id` = local self identity; `sender_contact_id` = `envelope.sender_contact_id`; remaining fields match signed envelope. Wrong AAD → hard fail.

## AEAD plaintext (E010)

Binary **`ChatPayload` v1** — AEAD plaintext. Layout: [WIRE_SCHEMAS § ChatPayload](WIRE_SCHEMAS.md#chatpayload-v1--binary-d087d088).

**Vector A fixture** (`text="Hello"`, plain default):

| Field | Value |
|-------|-------|
| **`bytes` (hex)** | `0100000000000000000548656c6c6f` |

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

| `e2e` / `e2e_public` | `{ "e2e": { "payload_b64": "…" } }` |
| `e2e_public` opening a session | `{ "e2e": { "payload_b64": "…", "key_init_b64": "…", "sender_keys": { …key card… } } }` |

`sender_keys` appears only with `key_init_b64`, must be the key card of `sender_contact_id`, and is checked on its own (it is signed by the account it names), so it is not part of the envelope signing bytes.

Reject envelopes with `thread_id`, `public_relay`, `body.content_b64`, or unknown `envelope_version`.

## Canonical relay-envelope signing bytes (E014)

Do **not** sign JSON dump. Build fixed binary bytes, then sign them with ML-DSA-65 (`EnvelopeSigner::BuildSignBytes`, `MlDsa::Sign`). String fields use **LenUtf8** (D088).

**Domain prefix** (34 bytes): `"pp-browser:relay-envelope-sign-v2"` + NUL.

Then: `sign_version=2`, `envelope_version=1`, `route_kind`, `channel`, `timestamp` (i64 BE), `sender_seq` (u64 BE), `session_epoch` (u32 BE), `body_hash` (32 bytes BLAKE2b-256), `message_id` LenUtf8, `sender_contact_id` LenUtf8, `sender_relay_id` LenUtf8, `route.group_id` LenUtf8 (empty when none), `body.e2e.key_init_b64` LenUtf8 (empty when none).

Everything the receiver acts on is signed: the blob it decrypts, the group thread it files into, the AutoKey session it may move to, and the relay route it may learn for the sender (learned only from envelopes whose signature verified).

**Body shape:** a `direct` / `control` envelope carries `payload_b64` only; a `group` envelope `member_payloads` only (and a `group_id`). Signer and parser refuse anything else, and unknown route kinds.

**Body hash:**

```
direct / control:  body_hash = BLAKE2b-256( 0x02 || LenBytes(decoded payload_b64) )
group:             body_hash = BLAKE2b-256( 0x02 || for each (member_id, blob) in member_payloads, by member_id:
                                                    LenUtf8(member_id) || LenBytes(decoded blob) )
```

(`LenBytes` / `LenUtf8` = u64 BE length + bytes.) The hash covers exactly what the receiver decrypts.

Only **`body_kind = 0x02`** (E2E ciphertext blob) — no plaintext path (D090).

`signature` on wire: base64 over the 3309-byte ML-DSA-65 signature. Signing is hedged (randomized), so there is no frozen signature vector: tests sign and verify; the sign bytes below are frozen.

Peer verify keys: `PeerSigningKeyStore` / `PeerKemKeyStore`, filled from account cards ([ACCOUNT_CARD.md](ACCOUNT_CARD.md#resolving-keys), E028). Inbound verify before decrypt. An account's signing key is kept only if it hashes to the Account ID it is filed under (self-certifying; `AccountIdFromMlDsaPublicKey`), whichever source offers it (card store, `sender_keys`, the peer's devices, a contact card, the directory), and a directory answer must be for the account asked. The account KEM key is kept only with the account's own binding (`foundation/crypto/KemKeyBinding.h`): its ML-DSA signature over domain `pp-browser:kem-binding-v1\0`, version u8 `1`, the raw ML-DSA-65 account public key and the raw ML-KEM-768 public key. The relay stores the binding from `register/finish` and returns it with every directory entry (`kem_binding_signature_b64`); a key without a valid binding is refused, so no carrier, the relay included, can substitute one.

## Send / receive pipeline (e2e)

1. Build binary `ChatPayload` → canonical AAD from envelope fields → `MessageCipher::Encrypt` → blob → base64 → `body.e2e.payload_b64`.
2. `EnvelopeSigner::BuildSignBytes` → sign.
3. Receive: when `body.e2e.sender_keys` is present, check it as the key card of `sender_contact_id` and keep it → verify signature → resolve PSK for `envelope.session_epoch` → decrypt with AAD from envelope → `ChatPayloadCodec::Decode` → ingest.

Outbound HKDF uses `chat_targets.session_epoch`; inbound uses `envelope.session_epoch` (E019).

## `base/crypto` module (target)

| Component | Role |
|-----------|------|
| `SessionKeyDeriver` | HKDF-SHA256 |
| `CanonicalAad` | Build/parse AAD |
| `MessageCipher` | AEAD encrypt/decrypt |
| `EncryptedPayload` | Blob codec + base64 |
| `ReplayWindow` | Seq acceptance helper (D013 classifier is authoritative) |
| `IPskSessionStore` | PSK + retired epochs + public `key_scope` / conversation KEM on `chat_targets` |

Implementation: [projects/e2e-message-crypto/PHASES.md](../../projects/e2e-message-crypto/PHASES.md) phase c1.

## Frozen test vectors

Regenerate: `projects/e2e-message-crypto/tools/gen_sign_vectors.py`, `gen_aead_vectors.py`, `chatpayload_codec.py`.

Regenerate after E023 channel enum change: `projects/e2e-message-crypto/tools/gen_sign_vectors.py`, `gen_aead_vectors.py`, `chatpayload_codec.py`.

### Sign bytes — `e2e` (private direct)

**Note:** E023 sets `e2e` → `channel = 0` in sign/AAD bytes. Regenerate fixtures before c1 tests lock.

| Field | Value |
|-------|-------|
| `message_id` | `660e8400-e29b-41d4-a716-446655440001` |
| `sender_contact_id` | `relay:alice123` |
| `timestamp` | `1719662400456` |
| `sender_seq` | `42` |
| `session_epoch` | `1` |
| **`payload_b64`** | `AQABAgMEBQYHCAkKCwwNDg8QERITFBUWF1vPScnCPAVe+dnJiV9kKBztMM3qj/Hi+RhfLy6wlhU=` |
| **`body_hash` (hex)** | `f046c5fdbe2e312864d2dc1386e8c63eda7062f3ce2a2c1abed3860a1a566f3c` |
| **`sign_bytes` (hex)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100010000019063ddd3c8000000000000002a00000001f046c5fdbe2e312864d2dc1386e8c63eda7062f3ce2a2c1abed3860a1a566f3c000000000000002436363065383430302d653239622d343164342d613731362d343436363535343430303031000000000000000e72656c61793a616c696365313233` |

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
