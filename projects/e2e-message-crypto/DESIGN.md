# Design — desired end state

## Principles

1. **Symmetric E2E for message bodies** — Relay and network observers see ciphertext on the `e2e` channel; confidentiality does not depend on relay trust.
2. **Manual key distribution v1** — 256-bit PSK exchanged out-of-band with fingerprint verification; no automated ECDH in c1–c3.
3. **Authenticated encryption only** — XChaCha20-Poly1305 with canonical AAD; never encrypt-then-MAC separately, never raw XOR.
4. **Align with chat-storage sync model** — `sender_seq`, `session_epoch`, and strict ingest ([D008–D014](../chat-storage-and-memory/DECISIONS.md)) bind to crypto AAD and key rotation.
5. **Classical + PQ layered threat model** — Symmetric layer is PQ-adequate; Ed25519 relay signatures are classical with a planned hybrid upgrade path.
6. **Storage abstraction** — `IPskSessionStore` seam; v1 backing store is `profile.db` `chat_targets` (E008/D084); keychain backend later.
7. **Implement in `base`**, wire in `feature` — Crypto module has no RmlUi or `P2pMessagingService` dependencies.

## Threat model

| Adversary capability | Protected by (v1) | Not protected (v1) |
|----------------------|-------------------|---------------------|
| Relay reads message body on `e2e` | AEAD ciphertext | Metadata: timestamps, sizes, traffic patterns |
| Relay forges E2E ciphertext without PSK | AEAD + seq in AAD | — |
| Relay forges envelope (wrong sender) | Ed25519 verify + pinned peer signing key (E016) | — |
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
- **On-disk:** `master_psk_b64`, `psk_fingerprint`, and `retired_psks_json` live on **`profile.db` → `chat_targets`** (E008/D084) — same PK as seq/epoch.

### Chat target identity (D056, D079)

Canonical **`ChatTargetKey`** — matches [chat-storage DESIGN § ChatTargetKey](../chat-storage-and-memory/DESIGN.md#chattargetkey-direct-p2p--d056-d079) and [D079](../chat-storage-and-memory/DECISIONS.md#d079--local-contact-vs-communicating-identity-identity-keyed-chattargetkey):

| Field | Notes |
|-------|-------|
| `peer_identity_kind` | `relay_user`, `peer_id`, … — v1 relay uses `relay_user` |
| `peer_identity_value` | Routable id string, e.g. `relay:abc123` (D082 / [E017](DECISIONS.md#e017--relay-user-identity-value-format)) |
| `channel` | `e2e` \| `public_relay` |

| Use | Key |
|-----|-----|
| C++ type | `ChatTargetKey{ peer_identity_kind, peer_identity_value, channel }` |
| `chat_targets` PK / PSK store key | `(peer_identity_kind, peer_identity_value, channel)` |
| Wire routing (inbound) | `{ sender_contact_id: identity value, route.channel }` + inferred kind → receiver's `ChatTargetKey` |

**`Contact.id`** (local address book) and **`local:self`** (local transcript sentinel) are **never** in AAD or relay envelope. Wire **`sender_contact_id`** = sender's **communicating identity value** (D079).

**`thread_id` / `local_thread_id` is never in AAD or relay envelope.**

### Peer signing keys (E016)

Envelope signatures (E014) bind envelope fields and body hash; they do **not** carry the sender's public key. **`EnvelopeSigner::Verify(envelope, public_key_b64)`** needs a local lookup:

```
(sender_contact_id, peer_identity_kind) → signing_public_key_b64
```

| Concept | Scope | Example |
|---------|-------|---------|
| **Communicating identity** | Wire routing (D079) | `relay_user` + `relay:abc…` |
| **Signing public key** | Ed25519 verify only | 32-byte key, base64 in store |
| **PSK** | E2E body AEAD only (E001) | Independent of signing key |

**Trust establishment (v1):**

1. **Directory** returns `signing_public_key_b64` on people search hits (relay already stores key at registration).
2. **Add contact** → persist in **`PeerSigningKeyStore`**; show BLAKE2b fingerprint for OOB compare with peer (same display rules as PSK — E011).
3. **Manual add** → user may paste `signing_public_key_b64` when directory is unavailable.
4. **Lazy fetch** — `GET /v1/users/{relay_user_id}` when a message arrives from an unknown `sender_contact_id` (D080 ephemeral public); cache then verify. Missing key → **hard reject** (same as invalid signature).

**Storage:** `profiles/{profile_id}/crypto/signing_keys.json` — map key `identity:{kind}:{value}` → `{ signing_public_key_b64, fingerprint }`. Not in AAD, not on wire, not in `Contact.id`.

**Directory wire (relay — v1 additions):**

Search hit (optional field on each hit):

```json
{
  "hit_id": "…",
  "display_name": "Alice",
  "nickname": "alice",
  "signing_public_key_b64": "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=",
  "ids": [{ "kind": "relay_user", "value": "relay:abc123", "primary": true }]
}
```

Lazy lookup: `GET /v1/users/{relay_user_id}` → `{ "relay_user_id": "…", "signing_public_key_b64": "…", "nickname": "…" }`.

**Do not** derive verify keys from `sender_relay_id` (unsigned metadata) or from a truncated base64 prefix of the key. **Do not** mix signing keys into `Contact.ids[]`.

**Rotation:** Same communicating identity + new signing key → update store entry. New `relay_user` id → new `ChatTargetKey` / thread (D079); historical messages verify with keys pinned per identity.

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

**Verify key lookup (receive — step 2, E016):**

1. Read `envelope.sender_contact_id` + inferred `peer_identity_kind` (v1: `relay_user`).
2. `PeerSigningKeyStore::Get(kind, value)` → `signing_public_key_b64`; on miss, `GET /v1/users/{value}` (relay), cache, retry once.
3. `EnvelopeSigner::Verify(envelope, signing_public_key_b64)` — failure → hard reject (D022); do not decrypt.

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

### PSK session store (v1 — E008/D084)

PSK material is **not** a separate JSON file. It lives on **`profile.db` → `chat_targets`** in the same row as `session_epoch` and `next_outgoing_seq` — see [chat-storage DESIGN § `profile.db` schema](../chat-storage-and-memory/DESIGN.md#profiledb-schema-v1).

| Column | Type | Notes |
|--------|------|-------|
| `master_psk_b64` | TEXT NULL | RFC 4648 base64, 32-byte key; `NULL` until PSK installed (`e2e` only) |
| `psk_fingerprint` | TEXT NULL | BLAKE2b-256 display string (E011); `NULL` when no PSK |
| `retired_psks_json` | TEXT NULL | JSON array of `{ epoch, master_psk_b64, retired_at }` after **`rotate_psk`** (E018); `NULL` or `[]` otherwise |

Example `retired_psks_json` value:

```json
[
  { "epoch": 1, "master_psk_b64": "…", "retired_at": 1719900000000 },
  { "epoch": 2, "master_psk_b64": "…", "retired_at": 1719980000000 }
]
```

**Decrypt lookup (E018):** `ResolveMasterPskForEpoch(epoch)` → `master_psk_b64` when `epoch == chat_targets.session_epoch`, else parse `retired_psks_json` for matching `epoch`, else error (hard reject on ingest).

**Log/test string key (not on disk):** `identity:{kind}:{value}|channel:{channel}` — human-readable `ChatTargetKey` label only.

### Chat-target seq state (chat-storage D047)

`next_outgoing_seq`, authoritative `session_epoch`, and **PSK columns** (`master_psk_b64`, `psk_fingerprint`, `retired_psks_json`) live in **`profile.db` → `chat_targets`** keyed by **`ChatTargetKey`** (D047/D084). **`local_thread_id`** is the current on-disk shell only (D056) — not on wire or in AAD. Epoch bump updates all `chat_targets` fields in one **`profile.db` transaction** ([chat-storage DESIGN § Epoch bump](../chat-storage-and-memory/DESIGN.md)).

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
| `IPskSessionStore.h` | Session CRUD + `ResolveMasterPskForEpoch(epoch)` (E018) — interface in `base/crypto` |
| `SqlitePskSessionStore.h/.cpp` | v1 impl in `feature/messaging/` — reads/writes `chat_targets` PSK columns (E008/D084) |

**Related (not in `base/crypto`):** **`PeerSigningKeyStore`** in `base/people/` — Ed25519 verify key cache per communicating identity (E016); uses same BLAKE2b fingerprint helper as PSK.

All public APIs return `Roe<T>` from `common/Error.h`.

## Key rotation and compromise

Aligned with [chat-storage D011/D038/D046](../chat-storage-and-memory/DECISIONS.md) (**v1:** rotate PSK or pause only — no continue-anyway):

1. Ingest detects **soft** integrity failure (seq conflict, rewind, repair failure, etc.) or **hard** wire/crypto failure.
2. **Soft:** pause ingest/outbound; UI shows choice sheet (D038) with disclosure. **Recommended:** manual new PSK exchange on **both peers**, then `session_epoch++` (innocent peer cannot decrypt until PSK is installed locally).
3. **Hard** (invalid signature, decrypt failure, epoch decrease): no override in v1; pause until delete thread or key rotation.
4. On **`rotate_psk`** path: `session_epoch++` via epoch bump transaction ([chat-storage DESIGN § Epoch bump](../chat-storage-and-memory/DESIGN.md#epoch-bump-transaction-d014-d068-cross-project), D068) — coordinator **appends retired PSK** to `chat_targets.retired_psks_json` (E018), cancels old-epoch pending outbox, then updates PSK + epoch in **`profile.db`** under writer mutex.
5. **No `epoch_start` system message** ([chat-storage D014](../chat-storage-and-memory/DECISIONS.md)) — first user message may use `sender_seq=1` in the new epoch.
6. HKDF uses new epoch for send; **decrypt** resolves `master_psk` by envelope `session_epoch` from `chat_targets` — active PSK or `retired_psks_json` (E018). Epoch-only bump (D014, same PSK) needs no retired entry. Local messages already persisted as plaintext (D048) stay readable without decrypt.
7. **No new ingest on old epoch after rotation** ([chat-storage DESIGN § Integrity recovery](../chat-storage-and-memory/DESIGN.md#integrity-recovery-d038)) — retired keys are for historical relay ciphertext (backfill, in-flight during bump), not live old-epoch traffic.

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
| `sender_contact_id` | `relay:alice123` |
| `route.kind` | `direct` → `route_kind = 0` |
| `route.channel` | `public_relay` → `channel = 0` |
| `timestamp` | `1719662400123` (Unix ms) |
| `sender_seq` | `0` (wire omits; signing uses zero) |
| `session_epoch` | `0` (wire omits; signing uses zero) |
| `body.content` | canonical JSON above |
| `body_hash` input | `0x01` \|\| canonical JSON bytes |
| **`body_hash` (hex)** | `db8f17cda6b57a0feff3b6aa09ca17e7ca15b32309cc85d555531c804e2c7f10` |
| **`sign_bytes` (hex, 143 bytes)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100000000019063ddd27b000000000000000000000000db8f17cda6b57a0feff3b6aa09ca17e7ca15b32309cc85d555531c804e2c7f10002435353065383430302d653239622d343164342d613731362d343436363535343430303030000e72656c61793a616c696365313233` |
| **`signature` (base64)** | `sgoePjY8ExAV+yVono5XyO6UUosHP0ka4Ham8f/2sKlUQwJvzbq1VFX+DWJlDVGZArw1MyPzQp44/H5+2zwGCA==` |

#### Vector 2 — `e2e`

| Input | Value |
|-------|-------|
| `message_id` | `660e8400-e29b-41d4-a716-446655440001` |
| `sender_contact_id` | `relay:alice123` |
| `route.kind` | `direct` → `route_kind = 0` |
| `route.channel` | `e2e` → `channel = 1` |
| `timestamp` | `1719662400456` (Unix ms) |
| `sender_seq` | `42` |
| `session_epoch` | `1` |
| `body.e2e.payload_b64` | `AQABAgMEBQYHCAkKCwwNDg8QERITFBUWF6q7qruqu6q7qruqu6q7qruqu6q7qruqu6q7qruqu6q7` |
| E2E blob (decoded, hex) | `01000102030405060708090a0b0c0d0e0f1011121314151617aabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabbaabb` |
| `body_hash` input | `0x02` \|\| decoded blob bytes |
| **`body_hash` (hex)** | `d32b5a0addb1b6980d44f511e4c6f6e09a7d32a3375e4f66a7de709afc4daeaf` |
| **`sign_bytes` (hex, 143 bytes)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100010000019063ddd3c8000000000000002a00000001d32b5a0addb1b6980d44f511e4c6f6e09a7d32a3375e4f66a7de709afc4daeaf002436363065383430302d653239622d343164342d613731362d343436363535343430303031000e72656c61793a616c696365313233` |
| **`signature` (base64)** | `teBg5BIfz/0qp4XslKHZRC2jIpD5N/JrWVVVFknLDLaJ8xVGcJ2JG/p8gd/qunjWNaNqcu5QI4Y0jnd+yU57BA==` |

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
