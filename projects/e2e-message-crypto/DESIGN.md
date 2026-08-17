# Design — desired end state

**Stable reference:** [docs/contracts/MESSAGE_ENCRYPTION.md](../../docs/contracts/MESSAGE_ENCRYPTION.md) — normative wire/crypto spec for agents and implementers. This file adds planning context, module map, and phase dependencies.

## Principles

1. **Symmetric E2E for all P2P message bodies** — Relay and network observers see ciphertext on **both direct tiers** (`e2e`, `e2e_public`) and on **group** messages (E021). Confidentiality does not depend on relay trust.
2. **Tier-appropriate key distribution** — **Private direct (`e2e`):** manual 256-bit PSK OOB with fingerprint verification (E011). **Public direct (`e2e_public`):** hybrid KEM PSK + signing-key resolver (E013/E024). **Group:** pairwise sender-keys (E022). No automated ECDH in c1–c3.
3. **Authenticated encryption only** — XChaCha20-Poly1305 with canonical AAD; never encrypt-then-MAC separately, never raw XOR.
4. **Align with chat-storage sync model** — `sender_seq`, `session_epoch`, and tier-specific ingest ([D008–D014](../chat-storage-and-memory/DECISIONS.md), [D089](../chat-storage-and-memory/DECISIONS.md#d089--three-chat-tiers-both-direct-tiers-e2e-e021)) bind to crypto AAD and key rotation.
5. **Classical + PQ layered threat model** — Symmetric layer is PQ-adequate; Ed25519 relay signatures are classical with a planned hybrid upgrade path.
6. **Storage abstraction** — `IPskSessionStore` seam; v1 backing store is `profile.db` `chat_targets` (E008/D084); keychain backend later.
7. **Implement in `base`**, wire in `feature` — Crypto module has no RmlUi or `P2pMessagingService` dependencies.

## Three chat tiers (E021 / D089)

Full tier policy matrix: [chat-storage DESIGN § Three chat tiers](../chat-storage-and-memory/DESIGN.md#three-chat-tiers-d089).

| Tier | Channel / route | Crypto phases |
|------|-----------------|---------------|
| Private direct | `e2e` | c1–c3 (manual PSK, strict ingest) |
| Public direct | `e2e_public` | After c3 + auto-key (E013/E024) |
| Group | `route.kind=group` | pairwise sender-keys (E022) |

Legacy **`public_relay`** is **not supported** (D090/E023).

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

**Out of scope c1–c3:** Group E2E (E022, later than c1–c3); forward secrecy without rotation policy; hiding message existence from relay.

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
| Relay envelope sig | Ed25519 (today) → **ML-DSA-65** | OpenSSL EVP → **mldsa-native** | Aggressive PQ (E025/E026) |
| Public auto-key | **ML-KEM-768** | **mlkem-native** | PQ-only (no X25519) |

## Key material

### Master PSK

- **Size:** 32 bytes (256 bits) from CSPRNG.
- **Private direct (`e2e`):** Out-of-band distribution — copy-paste, in-person, etc. **Initial setup (E011):** either peer generates; generating side **exports** raw base64 + fingerprint; peer **imports** paste. **`rotate_psk`:** initiator **exports** **`pp-browser-psk-bundle-v1`** JSON; innocent peer **imports** — active key + up to **`kMaxRetiredPskEpochs` (8)** retired epochs (E020/D086).
- **Public direct (`e2e_public`):** Hybrid KEM establishment (E013/E024) — peer agreement for `master_psk`; signing trust via **`IPeerSigningKeyResolver`** (relay v1, on-chain attestation `[later]`). No mandatory OOB PSK fingerprint before first send; optional verify deferred.
- **Verification (private tier):** Both parties display `fingerprint = BLAKE2b-256(master_psk)` as grouped hex; compare OOB, then user explicitly confirms before first **`e2e`** send. Persist **`psk_verified_at`** on `chat_targets`; clear on PSK replace/import/rotation.

**Initial establishment flow (epoch 1):**

```
Alice (starts Secure message)          Bob
  | Generate 32 bytes (CSPRNG)           |
  | Show base64 + fingerprint + Copy     |
  | Share OOB (Signal, in-person, …)     |
  |                                      | Import (paste base64)
  |                                      | Same fingerprint shown
  | Both confirm fingerprint OOB         |
  | Either may send first E2E message    |
```

No wire-protocol initiator — only UX default (Secure-message starter offers Generate first). Both peers MUST hold the same bytes before relying on E2E (E015).

### Public direct auto-key (`e2e_public` — E024 / O007)

**Two independent anchors** — see [E024](DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007):

| Anchor | Mechanism | Now | Later |
|--------|-----------|-----|-------------|
| **Signing** (who sent) | **`IPeerSigningKeyResolver`** → **`PeerSigningKeyStore`** | Relay directory + lazy fetch (E016) | On-chain attestation (CAIP-10 linked — D091), **chain-preferred** |
| **PSK** (body secrecy) | **ML-KEM-768** **account** KEM (E026 / **M015**; amends E013) | Encapsulate to the person | Same — relay never learns `master_psk` |

**PSK derivation from KEM:**

```
master_psk = HKDF-SHA256(
  ikm   = kem_shared_secret,
  salt  = "pp-browser-msg-v1",
  info  = "auto-key-v1|channel:e2e_public"
)
```

Then session keys use E015 (`channel:e2e_public|epoch:…`) from `master_psk` as today.

**First-message / auto-create path (D080):**

1. Recipient **account** publishes ML-KEM-768 on the directory (`kem_public_key_b64`). Linked devices share that secret (**M015**).
2. Initiator encapsulates to the recipient **account** KEM → `master_psk` → encrypts `body.e2e.payload_b64`.
3. When recipient may lack PSK, envelope includes optional **`body.e2e.key_init_b64`** — KEM encapsulation any linked install of that person can decapsulate at receive step 7.
4. Relay may store/forward `key_init_b64`; it MUST NOT generate, seal, or learn `master_psk`.

**Rejected:** directory-sealed PSK; relay as PSK broker; blockchain address as wire identity in v1.

**Device-lock / D2D rekey (E027):** Account-scope public chats do **not** auto-`rotate_psk`. Either side may send `psk_rotate` (system control) after the thread exists. New PSK is in `key_init_b64` wrapped to account KEM or conversation KEM — never under the old PSK. Quiet auto-`rotate_psk` only when both sides are device-bound. See [E027](DECISIONS.md#e027--public-11-device-lock-rekey-auto-rotate_psk-only-when-both-sides-are-device-bound).

**Module map (target):**

| Type | Location |
|------|----------|
| `IPeerSigningKeyResolver` | `src/base/messaging/` |
| `PeerSigningKeyStore` | `src/base/messaging/` or `src/base/people/` |
| `AutoKeyEstablishment` | `src/base/crypto/` |
| Ingest wiring | `src/feature/messaging/` receive pipeline step 2 + 7 |

### Session key derivation (E015)

```
session_key = HKDF-SHA256(
  ikm   = master_psk,
  salt  = "pp-browser-msg-v1",
  info  = "channel:{channel}|epoch:{session_epoch}"
)
```

- **`channel`:** `e2e` (private direct) or `e2e_public` (public direct) — both use derived keys for body encryption (D090).
- **`session_epoch`:** uint32, bumped on key rotation / compromise recovery ([chat-storage D014](../chat-storage-and-memory/DECISIONS.md)). New epoch → new `session_key`; seq resets to 1 for that epoch.
- **Pair scoping:** `master_psk` is unique per **`ChatTargetKey`** (one OOB secret per peer identity + channel). HKDF `info` intentionally omits identity strings so **both peers derive the same `session_key`** from the shared `master_psk` + `(channel, epoch)` — see [E015](DECISIONS.md#e015--hkdf-info-channel--epoch-only-option-a).
- **On-disk:** `master_psk_b64`, `psk_fingerprint`, `psk_verified_at` (E011), and `retired_psks_json` live on **`profile.db` → `chat_targets`** (E008/D084) — same PK as seq/epoch.

### Chat target identity (D056, D079)

Canonical **`ChatTargetKey`** — matches [chat-storage DESIGN § ChatTargetKey](../chat-storage-and-memory/DESIGN.md#chattargetkey-direct-p2p--d056-d079) and [D079](../chat-storage-and-memory/DECISIONS.md#d079--local-contact-vs-communicating-identity-identity-keyed-chattargetkey):

| Field | Notes |
|-------|-------|
| `peer_identity_kind` | `relay_user`, `peer_id`, … — v1 relay uses `relay_user` |
| `peer_identity_value` | Routable id string, e.g. `relay:abc123` (D082 / [E017](DECISIONS.md#e017--relay-user-identity-value-format)) |
| `channel` | `e2e` \| `e2e_public` |

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

1. **`IPeerSigningKeyResolver`** (E024) — composable backends; results cached in **`PeerSigningKeyStore`** with provenance (`source`, `source_ref`, `trusted_at`).
2. **Directory** returns `signing_public_key_b64` on people search hits (relay already stores key at registration).
3. **Add contact** → persist via resolver; show BLAKE2b fingerprint for OOB compare with peer (same display rules as PSK — E011).
4. **Manual add** → user may paste `signing_public_key_b64` when directory is unavailable.
5. **Lazy fetch** — `RelayDirectoryResolver` calls `GET /v1/users/{relay_user_id}` when a message arrives from an unknown `sender_contact_id` (D080 ephemeral public); cache then verify. Missing key → **hard reject** (same as invalid signature).
6. **`[later]`** — **`OnChainAttestationResolver`** confirms/overrides relay binding using CAIP-10-linked on-chain attestation (D091); **chain-preferred** when attestation exists.

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
| 1 | 1 | `channel` enum: `0` = `e2e`, `1` = `e2e_public` (E023) |
| | var | `peer_contact_id` — **LenUtf8** |
| | var | `message_id` — **LenUtf8** |
| | var | `sender_contact_id` — **LenUtf8** |
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

Binary **`ChatPayload` v1** ([chat-storage D087](../chat-storage-and-memory/DECISIONS.md#d087--binary-chatpayload-v1-e014-body_hash--e010-plaintext), [WIRE_SCHEMAS](../../docs/contracts/WIRE_SCHEMAS.md#chatpayload-v1--binary-d087)) — AEAD plaintext for all direct tiers.

**Vector A fixture** (`text="Hello"`, plain default):

| Field | Value |
|-------|-------|
| **`bytes` (hex)** | `0100000000000000000548656c6c6f` |

All `content_type` values (`text`, `annotation`, `contact_card`, `crypto_tx`, `system`) may appear inside E2E ciphertext. `content_rml` for AI rows remains app-local on `ThreadMessage` until a future payload extension.

**Size:** Decrypted plaintext must be ≤ **`kMaxE2ePlaintextBytes` (128 KiB)** ([chat-storage D029](../chat-storage-and-memory/DECISIONS.md)). Check byte length after decrypt, before `ChatPayloadCodec::Decode`.

## Encrypted payload blob

Binary layout placed inside relay body (base64-encoded for JSON):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `payload_version` = `1` |
| 1 | 24 | `nonce` (random, `randombytes_buf`) |
| 25 | var | ciphertext + Poly1305 tag (`crypto_aead_xchacha20poly1305_ietf`) |

Libsodium API: `crypto_aead_xchacha20poly1305_ietf_encrypt` / `_decrypt` with `npub` = nonce, `ad` = canonical AAD, `k` = `session_key` (32 bytes).

## Relay envelope integration (phase c2 — D056)

Outer envelope: JSON + Ed25519 signature. **No `thread_id`.** **`envelope_version: 1`** required (chat-storage D072). Normative shapes: [docs/contracts/WIRE_SCHEMAS.md](../../docs/contracts/WIRE_SCHEMAS.md). See [chat-storage DESIGN § Relay envelope](../chat-storage-and-memory/DESIGN.md#relay--direct-envelope-d056).

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
| `e2e` / `e2e_public` | `{ "e2e": { "payload_b64": "…" } }` | Full fields incl. `sender_seq`, `session_epoch` |

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
| `route.channel` | yes (`channel` u8 enum) | When direct: `0` = `e2e`, `1` = `e2e_public` (E023) |
| `timestamp` | yes (i64 BE) | Unix **milliseconds** |
| `body_hash` | yes (32 bytes) | BLAKE2b-256 — see below |
| `sender_seq` | yes (u64 BE) | Required on direct wire |
| `session_epoch` | yes (u32 BE) | Required on direct wire |
| `sender_relay_id` | **no** | Relay registration id only |
| `thread_id` | **no** | Legacy; reject on ingest |
| `signature` | **no** | |

Bump **`sign_version`** (first byte after domain prefix) to change hash algorithm or byte layout without necessarily changing relay JSON. Bump **`envelope_version`** when the signed **field set** changes (D072).

### Byte layout (`sign_version = 1`, `envelope_version = 1`)

Big-endian integers. String fields use **LenUtf8** (D088). **Sign bytes** = domain prefix || fixed header || LenUtf8 strings.

**Domain prefix** (34 bytes): UTF-8 `"pp-browser:relay-envelope-sign-v1"` + NUL (`0x00`).

| Offset (from start of sign bytes) | Size | Field |
|-----------------------------------|------|-------|
| 0 | 34 | domain prefix |
| 34 | 1 | `sign_version` = **`1`** |
| 35 | 1 | `envelope_version` = **`1`** |
| 36 | 1 | `route_kind`: **`0`** = direct, **`1`** = group |
| 37 | 1 | `channel`: **`0`** = `e2e`, **`1`** = `e2e_public` when `route_kind=direct`; **`0xFF`** reserved when `route_kind=group` (future) |
| 38 | 8 | `timestamp` (i64 BE, Unix ms) |
| 46 | 8 | `sender_seq` (u64 BE) |
| 54 | 4 | `session_epoch` (u32 BE) |
| 58 | 32 | `body_hash` (BLAKE2b-256 output) |
| 90 | var | `message_id` — **LenUtf8** |
| | var | `sender_contact_id` — **LenUtf8** |

**Group route:** under a new `envelope_version`, append **`group_id` LenUtf8** after `sender_contact_id` when `route_kind=group`.

### Body hash (`body_hash`)

```
body_hash = BLAKE2b-256( 0x02 || decoded_e2e_blob_bytes )
```

| Channel | `body_kind` | `payload_bytes` |
|---------|-------------|-----------------|
| `e2e` / `e2e_public` | `0x02` | Raw bytes from **base64 decode** of `body.e2e.payload_b64` |

Use libsodium **`crypto_generichash`** with 32-byte output. Only **`body_kind = 0x02`** — no plaintext path (D090/E023).

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

**Verify key lookup (receive — step 2, E016/E024):**

1. Read `envelope.sender_contact_id` + inferred `peer_identity_kind` (v1: `relay_user`).
2. `IPeerSigningKeyResolver::Resolve(kind, value)` → cache in **`PeerSigningKeyStore`** on success; **fail closed** if unresolved.
3. `EnvelopeSigner::Verify(envelope, signing_public_key_b64)` — failure → hard reject (D022); do not decrypt.

**Send pipeline (e2e):**

1. Build binary `ChatPayload` from `ThreadMessage` → `ChatPayloadCodec::Encode`.
2. Assign `(message_id, sender_seq)` at first local persist (chat-storage D010); stamp envelope with **`chat_targets.session_epoch`** (authoritative outbound epoch).
3. Build canonical AAD: `peer_contact_id` = recipient identity value from `ChatTargetKey`, `sender_contact_id` = local outbound identity for this thread, plus channel, ids, seq, epoch, timestamp.
4. `MessageCipher::Encrypt(chatpayload_bytes, session_key, aad)` → blob → base64 → `body.e2e.payload_b64`.
5. `EnvelopeSigner::BuildSignBytes` → `IdentityStore::SignPayload` (no `thread_id`).
6. Relay; on receive, verify signature → resolve `ChatTargetKey` → decrypt with **`envelope.session_epoch`** (E019) → **E2E D013 ingest**.

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
| `psk_verified_at` | INTEGER NULL | Unix ms when user confirmed OOB fingerprint match (E011); `NULL` until verified; cleared on PSK replace/import/rotation |
| `retired_psks_json` | TEXT NULL | JSON array of `{ epoch, master_psk_b64, retired_at }` after **`rotate_psk`** (E018); max **`kMaxRetiredPskEpochs` (8)** entries (D086/E020); `NULL` or `[]` otherwise |

Example `retired_psks_json` value:

```json
[
  { "epoch": 1, "master_psk_b64": "…", "retired_at": 1719900000000 },
  { "epoch": 2, "master_psk_b64": "…", "retired_at": 1719980000000 }
]
```

**Decrypt lookup (E018, E019/D085):** `ResolveMasterPskForEpoch(envelope.session_epoch)` — pass the **envelope epoch**, not `chat_targets.session_epoch`. Returns `master_psk_b64` when `envelope.session_epoch == chat_targets.session_epoch`, else parse `retired_psks_json` for matching `epoch`, else error (hard reject on ingest). HKDF `info` uses the same envelope epoch (E015).

**Log/test string key (not on disk):** `identity:{kind}:{value}|channel:{channel}` — human-readable `ChatTargetKey` label only.

### Rich OOB PSK bundle (E020/D086)

Used for **`rotate_psk`** export/import and multi-hop catch-up (O006). Not on the relay wire.

```json
{
  "format": "pp-browser-psk-bundle-v1",
  "channel": "e2e",
  "active_epoch": 3,
  "master_psk_b64": "…",
  "retired_epochs": [
    { "epoch": 1, "master_psk_b64": "…" },
    { "epoch": 2, "master_psk_b64": "…" }
  ]
}
```

| Operation | Behavior |
|-----------|----------|
| **Export** (initiator) | After local `rotate_psk`: `active_epoch`, new `master_psk_b64`, + up to **8** retired epochs — most recent tail before active |
| **Import** (innocent peer) | Validate (≤ **`kMaxPskBundleBytes` 4 KiB**); merge retired into `retired_psks_json`; set active PSK + `session_epoch`; reset seq; cancel old-epoch pending (D086) |
| **Initial setup** | Generate + export raw base64 (E011) on one peer; import on other ≡ bundle with `active_epoch: 1`, empty `retired_epochs[]` |

If peer rotated more than **8** times before import, epochs outside the retired tail may not decrypt from relay backfill — disclose on import.

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
| `IPskSessionStore.h` | Session CRUD + **`GenerateMasterPsk()`** (E011) + `ResolveMasterPskForEpoch(epoch)` (E018) + **`ImportPskBundle` / `ExportPskBundle`** (E020) + **`MarkPskVerified()`** / **`IsPskVerified()`** (E011) — interface in `base/crypto` |
| `SqlitePskSessionStore.h/.cpp` | v1 impl in `feature/messaging/` — reads/writes `chat_targets` PSK columns (E008/D084) |

**Related (not in `base/crypto`):** **`PeerSigningKeyStore`** in `base/people/` — Ed25519 verify key cache per communicating identity (E016); uses same BLAKE2b fingerprint helper as PSK.

All public APIs return `Roe<T>` from `common/Error.h`.

## Key rotation and compromise

Aligned with [chat-storage D011/D038/D046](../chat-storage-and-memory/DECISIONS.md) (**v1:** rotate PSK or pause only — no continue-anyway):

1. Ingest detects **soft** integrity failure (seq conflict, rewind, repair failure, etc.) or **hard** wire/crypto failure.
2. **Soft:** pause ingest/outbound; UI shows choice sheet (D038) with disclosure. **Recommended:** manual new PSK exchange on **both peers**, then `session_epoch++` — export/import **`pp-browser-psk-bundle-v1`** (E020/D086); innocent peer installs bundle before new-epoch decrypt.
3. **Hard** (invalid signature, decrypt failure, epoch decrease): no override in v1; pause until delete thread or key rotation.
4. On **`rotate_psk`** path: `session_epoch++` via epoch bump transaction ([chat-storage DESIGN § Epoch bump](../chat-storage-and-memory/DESIGN.md#epoch-bump-transaction-d014-d068-cross-project), D068) — coordinator **appends retired PSK** to `chat_targets.retired_psks_json` (E018), cancels old-epoch pending outbox, then updates PSK + epoch in **`profile.db`** under writer mutex.
5. **No `epoch_start` system message** ([chat-storage D014](../chat-storage-and-memory/DECISIONS.md)) — first user message may use `sender_seq=1` in the new epoch.
6. HKDF uses **`chat_targets.session_epoch`** for **outbound** send; **inbound decrypt** uses **`envelope.session_epoch`** for HKDF and `ResolveMasterPskForEpoch` (E019/D085). After passive adopt, both match. Epoch-only bump (D014, same PSK) needs no retired entry. Local messages already persisted as plaintext (D048) stay readable without decrypt.
7. **No new ingest on old epoch after rotation** ([chat-storage DESIGN § Integrity recovery](../chat-storage-and-memory/DESIGN.md#integrity-recovery-d038)) — retired keys are for historical relay ciphertext (backfill, in-flight during bump), not live old-epoch traffic.
8. **Passive adopt (D085):** when peer bumps first with **epoch-only** (D014), innocent device updates `chat_targets` on first successful ingest. **`rotate_psk`:** innocent peer installs **rich OOB bundle** (E020/D086) before decrypt; bundle sets `session_epoch` + retired ledger at import.
9. **Multi-hop rotation (O006/D086):** bundle retired tail (max 8 epochs) covers skipped intermediate keys; relay ciphertext outside the tail is not guaranteed decryptable.

**`[later]`** optional relaxed ingest (`ingest_policy=relaxed`, `continue_anyway`) — see [chat-storage DESIGN § Relaxed ingest](../chat-storage-and-memory/DESIGN.md#relaxed-ingest--continue-anyway--public-direct-and-group-d046); not on private (D046).

## Post-quantum migration (phase c4)

| Component | Pre-c4 | c4 (aggressive PQ) |
|-----------|--------|---------------------|
| E2E body | PSK + XChaCha20-Poly1305 | Unchanged |
| Manual PSK | OOB 256-bit | Unchanged (PQ-safe) |
| Optional automated setup | None / draft hybrid | **ML-KEM-768 only** (`mlkem-native`) → HKDF input |
| Relay / account signatures | Ed25519 | **ML-DSA-65 only** (`mldsa-native`); device ML-DSA-65 = Peer ID / Noise auth |
| libp2p transport | BoringSSL TLS / classical Noise | **`/noise-mlkem768/1.0.0`** (ML-KEM-768 XXkem) + device ML-DSA-65 identity |

Do **not** use X25519 or ECDH alone for automated key agreement (E026).

## Relationship to chat-storage-and-memory

| chat-storage phase | Dependency for E2E |
|--------------------|-------------------|
| v2b — channel split | Required before c2 (`ChatTargetKey` + `FindOrCreateDirectThread`) |
| v6 — `sender_seq`, `session_epoch` on envelope | Required before c2 |
| v6 — strict ingest D013 | Required for production E2E trust |
| v2b — “Secure message” UI | Requires c3 key import |

E2E crypto **c1** can proceed in parallel (no messaging types changed).

## Test vectors (required before d0 sign-off; asserted in c1 unit tests)

Frozen vectors in unit tests, [docs/contracts/MESSAGE_ENCRYPTION.md](../../docs/contracts/MESSAGE_ENCRYPTION.md), and this design. Regenerate Ed25519 fixtures with [`tools/gen_sign_vectors.py`](tools/gen_sign_vectors.py); AEAD/codec fixtures with [`tools/gen_aead_vectors.py`](tools/gen_aead_vectors.py); binary payload bytes with [`tools/chatpayload_codec.py`](tools/chatpayload_codec.py).

### Shared test keypair (TEST ONLY)

| Field | Value |
|-------|-------|
| Ed25519 private key (32 bytes, hex) | `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f` |
| Ed25519 public key (32 bytes, hex) | `03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8` |
| Ed25519 public key (base64) | `A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=` |

Do **not** use this keypair outside tests.

### Ed25519 envelope signing (E014)

**Binary `ChatPayload` v1 — Vector A** (see [WIRE_SCHEMAS § ChatPayload](../../docs/contracts/WIRE_SCHEMAS.md#chatpayload-v1--binary-d087d088)):

| Field | Value |
|-------|-------|
| **`bytes` (hex)** | `0100000000000000000548656c6c6f` |

#### Vector 1 — `e2e` (private direct)

**Note:** After E023, `e2e` → `channel = 0` in sign bytes (was `1` in pre-D090 fixtures). Regenerate with [`tools/gen_sign_vectors.py`](tools/gen_sign_vectors.py).

| Input | Value |
|-------|-------|
| `message_id` | `660e8400-e29b-41d4-a716-446655440001` |
| `sender_contact_id` | `relay:alice123` |
| `route.kind` | `direct` → `route_kind = 0` |
| `route.channel` | `e2e` → `channel = 0` |
| `timestamp` | `1719662400456` (Unix ms) |
| `sender_seq` | `42` |
| `session_epoch` | `1` |
| `body.e2e.payload_b64` | `AQABAgMEBQYHCAkKCwwNDg8QERITFBUWF1vPScnCPAVe+dnJiV9kKBztMM3qj/Hi+RhfLy6wlhU=` |
| E2E blob (decoded, hex) | `01000102030405060708090a0b0c0d0e0f10111213141516175bcf49c9c23c055ef9d9c9895f64281ced30cdea8ff1e2f9185f2f2eb09615` |
| `body_hash` input | `0x02` \|\| decoded blob bytes |
| **`body_hash` (hex)** | `b09daad4a14b17961c834c3b027c3d03ef49a0b1f3bffaa7c8c22da097a8042e` |
| **`sign_bytes` (hex, 159 bytes)** | `70702d62726f777365723a72656c61792d656e76656c6f70652d7369676e2d763100010100010000019063ddd3c8000000000000002a00000001b09daad4a14b17961c834c3b027c3d03ef49a0b1f3bffaa7c8c22da097a8042e000000000000002436363065383430302d653239622d343164342d613731362d343436363535343430303031000000000000000e72656c61793a616c696365313233` |
| **`signature` (base64)** | `QeHtXYUc4uQJ+1qCSYtRpabAI/kk7Mik04kqQVOKk+O7WWO64VPnnNUUeaTmEX3BSOconoKo1ZwFVNO+JoGACA==` |

E2E blob is the AEAD vector below (56 bytes): valid XChaCha20-Poly1305 ciphertext for binary `ChatPayload` Vector A.

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

### Symmetric crypto — AEAD / codec

Regenerate with [`tools/gen_aead_vectors.py`](tools/gen_aead_vectors.py). Uses libsodium `crypto_aead_xchacha20poly1305_ietf_*`.

#### Vector — Alice encrypt → Bob decrypt

Envelope fields align with Ed25519 vector 2 above. Alice (`relay:alice123`) sends to Bob (`relay:bob456`).

| Input | Value |
|-------|-------|
| `session_key` (hex) | `f7dab69eb0c862df230bc383c1dea363637a6caf2d46d7b57d1b45b5526a7358` (from HKDF vector) |
| `nonce` (hex, 24 bytes) | `000102030405060708090a0b0c0d0e0f1011121314151617` |
| `peer_contact_id` (Alice AAD) | `relay:bob456` |
| `sender_contact_id` | `relay:alice123` |
| `message_id` | `660e8400-e29b-41d4-a716-446655440001` |
| `sender_seq` | `42` |
| `session_epoch` | `1` |
| `timestamp` | `1719662400456` (Unix ms) |
| `channel` | `e2e` → `1` |
| **`aad` (hex, 108 bytes)** | `0101000000000000000c72656c61793a626f62343536000000000000002436363065383430302d653239622d343164342d613731362d343436363535343430303031000000000000000e72656c61793a616c696365313233000000000000002a000000010000019063ddd3c8` |
| `plaintext` (hex) | `0100000000000000000548656c6c6f` |
| **`ciphertext+tag` (hex, 31 bytes)** | `5bcf49c9c23c055ef9d9c9895f64281ced30cdea8ff1e2f9185f2f2eb09615` |

**Blob round-trip** (`EncryptedPayload` codec):

| Field | Value |
|-------|-------|
| **`blob` (hex, 56 bytes)** | `01000102030405060708090a0b0c0d0e0f10111213141516175bcf49c9c23c055ef9d9c9895f64281ced30cdea8ff1e2f9185f2f2eb09615` |
| **`payload_b64`** | `AQABAgMEBQYHCAkKCwwNDg8QERITFBUWF1vPScnCPAVe+dnJiV9kKBztMM3qj/Hi+RhfLy6wlhU=` |

**Cross-peer:** Bob decrypts with the same `aad` bytes after verifying `peer_contact_id == relay:bob456` (local self) and `sender_contact_id == envelope.sender_contact_id`. Both peers derive `session_key` from the shared HKDF `master_psk` fixture.

## Explicit non-goals

- Chaos-based or custom ciphers
- libsodium replacing BoringSSL for TLS/libp2p
- Group E2E / MLS in c1–c3
- Encrypting `identity.json` private keys (separate track)
- Forward secrecy without epoch rotation
