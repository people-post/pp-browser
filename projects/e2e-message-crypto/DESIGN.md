# Design — desired end state

## Principles

1. **Symmetric E2E for message bodies** — Relay and network observers see ciphertext on the `e2e` channel; confidentiality does not depend on relay trust.
2. **Manual key distribution v1** — 256-bit PSK exchanged out-of-band with fingerprint verification; no automated ECDH in c1–c3.
3. **Authenticated encryption only** — XChaCha20-Poly1305 with canonical AAD; never encrypt-then-MAC separately, never raw XOR.
4. **Align with chat-storage sync model** — `sender_seq`, `session_epoch`, and strict ingest ([D008–D014](../chat-storage-and-memory/DECISIONS.md)) bind to crypto AAD and key rotation.
5. **Classical + PQ layered threat model** — Symmetric layer is PQ-adequate; Ed25519 relay signatures are classical with a planned hybrid upgrade path.
6. **Storage abstraction** — `IPskSessionStore` seam; JSON default; keychain backend later.
7. **Implement in `base`**, wire in `feature` — Crypto module has no RmlUi or `P2pMessagingService` dependencies.

## Threat model

| Adversary capability | Protected by (v1) | Not protected (v1) |
|----------------------|-------------------|---------------------|
| Relay reads message body on `e2e` | AEAD ciphertext | Metadata: timestamps, sizes, traffic patterns |
| Relay forges E2E ciphertext without PSK | AEAD + seq in AAD | — |
| Network replay of captured E2E blob | `sender_seq` in AAD + ingest rules (chat-storage D013) | — |
| Classical break of Ed25519 identity | — | Relay envelope signatures (upgrade in c4) |
| Future CRQC breaks EC signatures | — | Ed25519 verify on relay; plan ML-DSA hybrid |
| Future CRQC harvest-now-decrypt-later on **symmetric** E2E | 256-bit PSK + XChaCha20 | — (if PSK established OOB as random 256 bits) |
| Local disk theft | — | PSK in JSON store until keychain (E008) |

**Out of scope v1:** Group MLS, forward secrecy without manual rotation, hiding message existence from relay.

## Crypto stack

```
┌─────────────────────────────────────────────────────────────┐
│ Application (feature/messaging) — phase c2+                 │
│   encrypt on send / decrypt on poll / branch on channel       │
├─────────────────────────────────────────────────────────────┤
│ base/crypto (libsodium) — phase c1                          │
│   MessageCipher · SessionKeyDeriver · CanonicalAad          │
│   EncryptedPayloadCodec · ReplayWindow · IPskSessionStore   │
├─────────────────────────────────────────────────────────────┤
│ Classical identity (BoringSSL/OpenSSL) — existing             │
│   Ed25519Signer — relay envelope + registration               │
├─────────────────────────────────────────────────────────────┤
│ Transport TLS (BoringSSL) — existing, unrelated to E2E body │
│   curl HTTPS · libp2p TLS · lsquic                            │
└─────────────────────────────────────────────────────────────┘
```

| Layer | Algorithm | Library | PQ note |
|-------|-----------|---------|---------|
| Message body | XChaCha20-Poly1305 | libsodium | PQ-adequate (256-bit keys) |
| Key derivation | HKDF-SHA256 | libsodium | PQ-adequate |
| PSK fingerprint | BLAKE2b-256 | libsodium | PQ-adequate |
| Master PSK | 32 random bytes | libsodium `randombytes_buf` | Must be full entropy |
| Relay envelope sig | Ed25519 | OpenSSL EVP (today) | Classical — c4 hybrid |
| Future key agreement | X25519 + ML-KEM-768 | liboqs or OQS provider (c4) | Hybrid KEM |

## Key material

### Master PSK

- **Size:** 32 bytes (256 bits) from CSPRNG.
- **Distribution:** Out-of-band — in-person QR, copy-paste over an already-trusted channel, etc.
- **Verification:** Both parties display `fingerprint = BLAKE2b-256(master_psk)` as grouped hex (e.g. `a1b2-c3d4-…`); must match before sending E2E content.

### Session key derivation (E015)

```
session_key = HKDF-SHA256(
  ikm   = master_psk,
  salt  = "pp-browser-msg-v1",
  info  = "channel:{channel}|epoch:{session_epoch}"
)
```

- **`channel`:** `e2e` only uses derived keys for body encryption; `public_relay` has no PSK session.
- **`session_epoch`:** uint32, bumped on key rotation / compromise recovery ([chat-storage D014](../chat-storage-and-memory/DECISIONS.md)). New epoch → new `session_key`; seq resets to 1 for that epoch.
- **Pair scoping:** `master_psk` is unique per **`ChatTargetKey`** (one OOB secret per peer identity + channel). HKDF `info` intentionally omits identity strings so **both peers derive the same `session_key`** from the shared `master_psk` + `(channel, epoch)` — see [E015](DECISIONS.md#e015--hkdf-info-channel--epoch-only-option-a).
- **`sessions.json` map key** (`identity:{kind}:{value}|channel:{channel}`) is storage/index only — not part of HKDF `info`.

### Chat target identity (D056, D079)

Canonical **`ChatTargetKey`** — matches [chat-storage DESIGN § ChatTargetKey](../chat-storage-and-memory/DESIGN.md#chattargetkey-direct-p2p--d056-d079) and [D079](../chat-storage-and-memory/DECISIONS.md#d079--local-contact-vs-communicating-identity-identity-keyed-chattargetkey):

| Field | Notes |
|-------|-------|
| `peer_identity_kind` | `relay_user`, `peer_id`, … — v1 relay uses `relay_user` |
| `peer_identity_value` | Routable id string, e.g. `relay:user:abc` |
| `channel` | `e2e` \| `public_relay` |

| Use | Key |
|-----|-----|
| C++ type | `ChatTargetKey{ peer_identity_kind, peer_identity_value, channel }` |
| `sessions.json` map key | `identity:{kind}:{value}|channel:{channel}` |
| `chat_targets` PK | `(peer_identity_kind, peer_identity_value, channel)` |
| Wire routing (inbound) | `{ sender_contact_id: identity value, route.channel }` + inferred kind → receiver's `ChatTargetKey` |

**`Contact.id`** (local address book) and **`local:self`** (local transcript sentinel) are **never** in AAD or relay envelope. Wire **`sender_contact_id`** = sender's **communicating identity value** (D079).

**`thread_id` / `local_thread_id` is never in AAD or relay envelope.**

## AEAD: associated data (canonical layout)

Fixed byte order (big-endian integers). **`aad_version = 1`** is the only AAD layout (D016 — no dual-version parser).

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `aad_version` = `1` |
| 1 | 1 | `channel` enum: `0` = `public_relay`, `1` = `e2e` |
| | 2 | `peer_contact_id_len` (u16 BE) |
| | var | `peer_contact_id` UTF-8 — recipient's **communicating identity value** (`ChatTargetKey.peer_identity_value` from **sender's** view; AAD field name is historical) |
| | 2 | `message_id_len` (u16 BE) |
| | var | `message_id` UTF-8 |
| | 2 | `sender_contact_id_len` (u16 BE) |
| | var | `sender_contact_id` UTF-8 — sender's **communicating identity value** (same as envelope `sender_contact_id`, D079) |
| | 8 | `sender_seq` (u64 BE) |
| | 4 | `session_epoch` (u32 BE) |
| | 8 | `timestamp` (i64 BE) |

**Rules:**

- **Sender** builds AAD with `peer_contact_id` = recipient's **communicating identity value**, `sender_contact_id` = sender's **communicating identity value** (fixed for the thread — D079).
- **Receiver** verifies before decrypt:
  - `peer_contact_id` = **local self** communicating identity value (this profile's outbound identity for the thread transport — e.g. own `relay_user` id);
  - `sender_contact_id` = `envelope.sender_contact_id` = thread **`ChatTargetKey.peer_identity_value`**;
  - `channel`, `message_id`, `sender_seq`, `session_epoch`, `timestamp` = corresponding **envelope** fields (after signature verify).
- `sender_seq` must match outer signed envelope and local `ThreadMessage` for `relay_visible` rows.
- Decrypt with wrong AAD → MUST fail (no silent ignore).
- Local-only rows (`relay_visible=false`) are not encrypted for relay. **`local:self`** is never in AAD.

## AEAD: plaintext (inside ciphertext — E010)

UTF-8 JSON serialization of **`ChatPayload`** ([chat-storage D026](../chat-storage-and-memory/DECISIONS.md)):

```json
{
  "schema_version": 1,
  "content_type": "text",
  "text": "Hello",
  "payload": {}
}
```

All `content_type` values (`text`, `annotation`, `contact_card`, `crypto_tx`, `system`) may appear inside E2E ciphertext. `content_rml` for AI rows remains app-local on `ThreadMessage` until a future payload extension.

**Size:** Decrypted plaintext must be ≤ **`kMaxE2ePlaintextBytes` (128 KiB)** ([chat-storage D029](../chat-storage-and-memory/DECISIONS.md)). Check byte length after decrypt, before `nlohmann::json::parse`.

## Encrypted payload blob

Binary layout placed inside relay body (base64-encoded for JSON):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `payload_version` = `1` |
| 1 | 24 | `nonce` (random, `randombytes_buf`) |
| 25 | var | ciphertext + Poly1305 tag (`crypto_aead_xchacha20poly1305_ietf`) |

Libsodium API: `crypto_aead_xchacha20poly1305_ietf_encrypt` / `_decrypt` with `npub` = nonce, `ad` = canonical AAD, `k` = `session_key` (32 bytes).

## Relay envelope integration (phase c2 — D056)

Outer envelope: JSON + Ed25519 signature. **No `thread_id`.** **`envelope_version: 1`** required (chat-storage D072). Normative shapes: [WIRE_SCHEMAS.md](../chat-storage-and-memory/WIRE_SCHEMAS.md). See [chat-storage DESIGN § Relay envelope](../chat-storage-and-memory/DESIGN.md#relay--direct-envelope-d056).

```json
{
  "envelope_version": 1,
  "message_id": "uuid",
  "sender_relay_id": "relay:…",
  "sender_contact_id": "relay:user:alice",
  "route": { "kind": "direct", "channel": "e2e" },
  "sender_seq": 42,
  "session_epoch": 1,
  "body": { "e2e": { "payload_b64": "…" } },
  "timestamp": 1719662400123,
  "signature": "…"
}
```

| Channel | `body` shape | Signed (via canonical bytes — E014) |
|---------|--------------|-------------------------------------|
| `public_relay` | `{ "content": { …ChatPayload… } }` | `envelope_version`, `message_id`, `sender_contact_id`, `route`, `timestamp`, `body_hash`; `sender_seq=0`, `session_epoch=0` |
| `e2e` | `{ "e2e": { "payload_b64": "…" } }` | Same + `sender_seq`, `session_epoch` from envelope |

**Not signed:** `thread_id`, `sender_relay_id`, `signature`, unknown top-level keys (D073).

**Reject** envelopes containing `thread_id` (legacy — D016). Reject unknown **`envelope_version`** (D072).

## Ed25519: canonical signing bytes

Decision **E014**. **Do not** sign `nlohmann::json::dump()` of the envelope. Build fixed binary bytes, then `Ed25519Signer::Sign(sign_bytes, private_key)`.

### Signed field set

| Field | In sign bytes | Wire notes |
|-------|---------------|------------|
| `envelope_version` | yes (u8) | Must be **1** in v1 |
| `message_id` | yes (length-prefixed UTF-8) | UUID string |
| `sender_contact_id` | yes (length-prefixed UTF-8) | Sender communicating identity **value** (D079) |
| `route.kind` | yes (`route_kind` u8 enum) | `0` = direct, `1` = group (future) |
| `route.channel` | yes (`channel` u8 enum) | When direct: `0` = public_relay, `1` = e2e |
| `timestamp` | yes (i64 BE) | Unix **milliseconds** |
| `body_hash` | yes (32 bytes) | BLAKE2b-256 — see below |
| `sender_seq` | yes (u64 BE) | **`0`** when `channel=public_relay` (wire omits field — D045) |
| `session_epoch` | yes (u32 BE) | **`0`** when `channel=public_relay` |
| `sender_relay_id` | **no** | Relay registration id only |
| `thread_id` | **no** | Legacy; reject on ingest |
| `signature` | **no** | |

Bump **`sign_version`** (first byte after domain prefix) to change hash algorithm or byte layout without necessarily changing relay JSON. Bump **`envelope_version`** when the signed **field set** changes (D072).

### Byte layout (`sign_version = 1`, `envelope_version = 1`)

Big-endian integers. Length-prefixed UTF-8 strings use **u16 BE** length (max 65535; UUIDs and contact ids fit).

**Sign bytes** = domain prefix || fixed header || length-prefixed strings.

**Domain prefix** (34 bytes): UTF-8 `"pp-browser:relay-envelope-sign-v1"` + NUL (`0x00`).

| Offset (from start of sign bytes) | Size | Field |
|-----------------------------------|------|-------|
| 0 | 34 | domain prefix |
| 34 | 1 | `sign_version` = **`1`** |
| 35 | 1 | `envelope_version` = **`1`** |
| 36 | 1 | `route_kind`: **`0`** = direct, **`1`** = group |
| 37 | 1 | `channel`: **`0`** = public_relay, **`1`** = e2e when `route_kind=direct`; **`0xFF`** reserved when `route_kind=group` (future) |
| 38 | 8 | `timestamp` (i64 BE, Unix ms) |
| 46 | 8 | `sender_seq` (u64 BE) |
| 54 | 4 | `session_epoch` (u32 BE) |
| 58 | 32 | `body_hash` (BLAKE2b-256 output) |
| 90 | 2 | `message_id_len` (u16 BE) |
| 92 | var | `message_id` (UTF-8) |
| | 2 | `sender_contact_id_len` (u16 BE) |
| | var | `sender_contact_id` (UTF-8) |

**`[post-v1]` group route:** under a new `envelope_version`, append length-prefixed `group_id` UTF-8 after `sender_contact_id` when `route_kind=group`.

### Body hash (`body_hash`)

```
body_hash = BLAKE2b-256( body_kind || payload_bytes )
```

| `channel` | `body_kind` | `payload_bytes` |
|-----------|-------------|-------------------|
| `public_relay` | `0x01` | Canonical UTF-8 JSON of **`body.content`** (`ChatPayload`) — same rules as `ChatPayloadCodec` / `chat_payload_json` (D069/D078) |
| `e2e` | `0x02` | Raw bytes from **base64 decode** of `body.e2e.payload_b64` (`[payload_version:1][nonce:24][ciphertext+tag]`) |

Use libsodium **`crypto_generichash`** with 32-byte output. The 1-byte `body_kind` prefix domain-separates public JSON from E2E binary inside the hash input.

### Signature on the wire

- Algorithm: **Ed25519** (OpenSSL EVP / existing `Ed25519Signer`).
- **`signature` field:** standard **base64** (RFC 4648, padded) over the 64-byte raw signature — v1 only; no hex.

### `EnvelopeSigner` (target — `base/messaging`)

Shared by relay send, relay poll verify, and c1/c2 test vectors. Lives in **`src/base/messaging/`** (not `base/crypto` — no AEAD dependency).

| API | Role |
|-----|------|
| `EnvelopeSigner::BuildSignBytes(envelope)` | Full binary signing input |
| `EnvelopeSigner::BodyHash(channel, body)` | BLAKE2b step |
| `EnvelopeSigner::Verify(envelope, public_key_b64)` | Rebuild bytes + `Ed25519Signer::Verify` |

`IdentityStore::SignPayload` becomes a thin wrapper: sign `BuildSignBytes` output.

**Send pipeline (e2e):**

1. Build `ChatPayload` JSON from `ThreadMessage`.
2. Assign `(message_id, sender_seq)` at first local persist (chat-storage D010).
3. Build canonical AAD: `peer_contact_id` = recipient identity value from `ChatTargetKey`, `sender_contact_id` = local outbound identity for this thread, plus channel, ids, seq, epoch, timestamp.
4. `MessageCipher::Encrypt(utf8(payload_json), session_key, aad)` → blob → base64 → `body.e2e.payload_b64`.
5. `EnvelopeSigner::BuildSignBytes` → `IdentityStore::SignPayload` (no `thread_id`).
6. Relay; on receive, verify signature → resolve `ChatTargetKey` → decrypt → **E2E D013 ingest**.

## Replay protection

Two layers:

1. **Cryptographic:** `sender_seq` in AAD — reusing ciphertext from another message fails decrypt or ingest.
2. **Protocol:** `ReplayWindow` in `base/crypto` is a **helper only** — holds out-of-order slots during gap repair. The feature-layer **D013 classifier** ([chat-storage D013/D020](../chat-storage-and-memory/DECISIONS.md)) is **authoritative** for accept/reject/compromise; `ReplayWindow` does not persist or override policy.

`ReplayWindow` (per `chat_target`, `sender_contact_id`, `session_epoch`):

- Accept strictly increasing `sender_seq` above last contiguous (with sliding window for benign reorder during repair).
- Reject `sender_seq <= last_accepted` outside duplicate-ID exception.

## On-disk layout

### PSK session store (v1)

```
{data_dir}/profiles/{profile_id}/crypto/sessions.json
```

```json
{
  "schema_version": 1,
  "sessions": {
    "identity:relay_user:relay:c1|channel:e2e": {
      "master_psk_b64": "…",
      "session_epoch": 1,
      "fingerprint": "a1b2-c3d4-e5f6-…"
    }
  }
}
```

### Chat-target seq state (chat-storage D047)

`next_outgoing_seq` and authoritative `session_epoch` live in **`profile.db` → `chat_targets`** keyed by **`ChatTargetKey`**. **`local_thread_id`** is the current on-disk shell only (D056) — not on wire or in AAD. Crypto **`sessions.json`** holds `session_epoch` for HKDF; **epoch bump** updates `sessions.json` + `chat_targets` in one transaction ([chat-storage DESIGN § Epoch bump](../chat-storage-and-memory/DESIGN.md)).

## `base/crypto` module (target)

| File | Role |
|------|------|
| `CryptoTypes.h` | `ChatTargetKey`, byte aliases |
| `CryptoConstants.h` | Protocol versions, HKDF labels, replay window size |
| `PskFingerprint.h/.cpp` | BLAKE2b-256 display formatting |
| `SessionKeyDeriver.h/.cpp` | HKDF-SHA256 |
| `CanonicalAad.h/.cpp` | Build/parse AAD bytes |
| `MessageCipher.h/.cpp` | AEAD encrypt/decrypt |
| `EncryptedPayload.h/.cpp` | Blob codec + base64 |
| `ReplayWindow.h/.cpp` | Seq acceptance helper |
| `IPskSessionStore.h` | Session CRUD interface |
| `JsonPskSessionStore.h/.cpp` | JSON persistence |

All public APIs return `Roe<T>` from `common/Error.h`.

## Key rotation and compromise

Aligned with [chat-storage D011/D038/D046](../chat-storage-and-memory/DECISIONS.md) (**v1:** rotate PSK or pause only — no continue-anyway):

1. Ingest detects **soft** integrity failure (seq conflict, rewind, repair failure, etc.) or **hard** wire/crypto failure.
2. **Soft:** pause ingest/outbound; UI shows choice sheet (D038) with disclosure. **Recommended:** manual new PSK exchange on **both peers**, then `session_epoch++` (innocent peer cannot decrypt until PSK is installed locally).
3. **Hard** (invalid signature, decrypt failure, epoch decrease): no override in v1; pause until delete thread or key rotation.
4. On **rotate_psk** path: `session_epoch++` via epoch bump transaction ([chat-storage DESIGN § Epoch bump](../chat-storage-and-memory/DESIGN.md#epoch-bump-transaction-d014-d068-cross-project), D068) — coordinator cancels old-epoch pending outbox, then updates `sessions.json` + `chat_targets` under `profile.db` mutex.
5. **No `epoch_start` system message** ([chat-storage D014](../chat-storage-and-memory/DECISIONS.md)) — first user message may use `sender_seq=1` in the new epoch.
6. HKDF uses new epoch; old epoch keys retained for decrypting historical messages locally.

**`[post-v1]`** optional relaxed ingest (`ingest_policy=relaxed`, `continue_anyway`) — see [chat-storage DESIGN § Relaxed ingest](../chat-storage-and-memory/DESIGN.md#post-v1-relaxed-ingest--continue-anyway-d046-extension); not in v1 (D046).

## Post-quantum migration (phase c4 — deferred)

| Component | v1 (c1–c3) | c4 target |
|-----------|------------|-----------|
| E2E body | PSK + XChaCha20-Poly1305 | Unchanged |
| Manual PSK | OOB 256-bit | Unchanged (PQ-safe) |
| Optional automated setup | None | Hybrid **X25519 + ML-KEM-768** → HKDF input |
| Relay signatures | Ed25519 | **Hybrid Ed25519 + ML-DSA-65** or PQ-only new identities |
| libp2p transport | BoringSSL TLS | Follow libp2p / industry PQ TLS when available |

Do **not** use X25519 or ECDH alone for automated key agreement after c4 without ML-KEM hybrid.

## Relationship to chat-storage-and-memory

| chat-storage phase | Dependency for E2E |
|--------------------|-------------------|
| v2b — channel split | Required before c2 (`ChatTargetKey` + `FindOrCreateDirectThread`) |
| v6 — `sender_seq`, `session_epoch` on envelope | Required before c2 |
| v6 — strict ingest D013 | Required for production E2E trust |
| v2b — “Secure message” UI | Requires c3 key import |

E2E crypto **c1** can proceed in parallel (no messaging types changed).

## Test vectors (required before c1 exit)

Frozen vectors in unit tests and this design. Regenerate Ed25519 fixtures with [`tools/gen_sign_vectors.py`](tools/gen_sign_vectors.py).

### Shared test keypair (TEST ONLY)

| Field | Value |
|-------|-------|
| Ed25519 private key (32 bytes, hex) | `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f` |
| Ed25519 public key (32 bytes, hex) | `03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8` |
| Ed25519 public key (base64) | `A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=` |

Do **not** use this keypair outside tests.

### Ed25519 envelope signing (E014)

**Canonical `ChatPayload` JSON** (v1 test fixture — minified UTF-8, keys in order: `schema_version`, `content_type`, `text`, `payload`):

```json
{"schema_version":1,"content_type":"text","text":"Hello","payload":{}}
```

#### Vector 1 — `public_relay`

| Input | Value |
|-------|-------|
| `message_id` | `550e8400-e29b-41d4-a716-446655440000` |
| `sender_contact_id` | `relay:user:alice` |
| `route.kind` | `direct` → `route_kind = 0` |
| `route.channel` | `public_relay` → `channel = 0` |
| `timestamp` | `1719662400123` (Unix ms) |
| `sender_seq` | `0` (wire omits; signing uses zero) |
| `session_epoch` | `0` (wire omits; signing uses zero) |
| `body.content` | canonical JSON above |
| `body_hash` input | `0x01` \|\| canonical JSON bytes |
| **`body_hash` (hex)** | `db8f17cda6b57a0feff3b6aa09ca17e7ca15b32309cc85d555531c804e2c7f10` |
| **`sign_bytes` (hex, 146 bytes)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100000000019063ddd27b000000000000000000000000db8f17cda6b57a0feff3b6aa09ca17e7ca15b32309cc85d555531c804e2c7f10002435353065383430302d653239622d343164342d613731362d343436363535343430303030001072656c61793a757365723a616c696365` |
| **`signature` (base64)** | `cAtYF/Zs/O663qTNQztUujP/ldJpcNOnV5LR8bAXvFAnuj+DX/9aD/THN1F3sUn5hnHE+W90xxipN/xRpyxlDg==` |

#### Vector 2 — `e2e`

| Input | Value |
|-------|-------|
| `message_id` | `660e8400-e29b-41d4-a716-446655440001` |
| `sender_contact_id` | `relay:user:alice` |
| `route.kind` | `direct` → `route_kind = 0` |
| `route.channel` | `e2e` → `channel = 1` |
| `timestamp` | `1719662400456` (Unix ms) |
| `sender_seq` | `42` |
| `session_epoch` | `1` |
| `body.e2e.payload_b64` | `AQABAgMEBQYHCAkKCwwNDg8QERITFBUWF6q7qruqu6q7qruqu6q7qruqu6q7qruqu6q7qruqu6q7` |
| E2E blob (decoded, hex) | `01000102030405060708090a0b0c0d0e0f1011121314151617aabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabb` |
| `body_hash` input | `0x02` \|\| decoded blob bytes |
| **`body_hash` (hex)** | `d32b5a0addb1b6980d44f511e4c6f6e09a7d32a3375e4f66a7de709afc4daeaf` |
| **`sign_bytes` (hex, 146 bytes)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100010000019063ddd3c8000000000000002a00000001d32b5a0addb1b6980d44f511e4c6f6e09a7d32a3375e4f66a7de709afc4daeaf002436363065383430302d653239622d343164342d613731362d343436363535343430303031001072656c61793a757365723a616c696365` |
| **`signature` (base64)** | `nwtJJnnidjH0TpCi2I8X4BhVc0Fzc4NkZZNa0JUb0S53WHxLsD8ClU3I60IGVGHfgZxQEhQSVqXgcXjrBwOrAw==` |

E2E blob layout for this fixture: `[payload_version=0x01][nonce=0x00..0x17][ciphertext+tag=0xAABB×16]` (57 bytes total). Content is arbitrary test material — not a valid AEAD ciphertext.

### Symmetric crypto — HKDF (E015)

| Input | Value |
|-------|-------|
| `master_psk` (hex, 32 bytes) | `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f` |
| `salt` | `pp-browser-msg-v1` |
| `channel` | `e2e` |
| `session_epoch` | `1` |
| HKDF `info` | `channel:e2e\|epoch:1` |
| **`session_key` (hex)** | `f7dab69eb0c862df230bc383c1dea363637a6caf2d46d7b57d1b45b5526a7358` |

Both peers with the same `master_psk` for a `ChatTargetKey` must derive this key — identity strings are not in HKDF `info`.

### Symmetric crypto — AEAD / codec (c1 — TBD at implementation)

Fill when `base/crypto` lands:

- One AEAD tuple: `session_key`, `nonce`, `aad` (hex), `plaintext`, `ciphertext` (hex)
- One full blob round-trip: binary → base64 → binary
- Cross-peer round-trip: Alice encrypt → Bob decrypt (shared `master_psk`, AAD built from envelope fields)

## Explicit non-goals

- Chaos-based or custom ciphers
- libsodium replacing BoringSSL for TLS/libp2p
- Group E2E / MLS in c1–c3
- Encrypting `identity.json` private keys (separate track)
- Forward secrecy without epoch rotation
