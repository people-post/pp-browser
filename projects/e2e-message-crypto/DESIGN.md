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

### Session key derivation

```
session_key = HKDF-SHA256(
  ikm   = master_psk,
  salt  = "pp-browser-msg-v1",
  info  = "contact:{contact_id}|channel:{channel}|epoch:{session_epoch}"
)
```

- **`channel`:** `e2e` only uses derived keys for body encryption; `public_relay` has no PSK session.
- **`session_epoch`:** uint32, bumped on key rotation / compromise recovery ([chat-storage D014](../chat-storage-and-memory/DECISIONS.md)). New epoch → new `session_key`; seq resets to 1 for that epoch.

### Chat target identity

Matches chat-storage thread identity boundary:

| Key | Fields |
|-----|--------|
| `ChatTargetKey` | `contact_id`, `channel` (`e2e` \| `public_relay`) |
| Store map key (string) | `contact:{id}|channel:{channel}` |

## AEAD: associated data (canonical layout)

Fixed byte order (big-endian integers). **Any change bumps protocol version.**

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `aad_version` = `1` |
| 1 | 2 | `thread_id_len` (u16 BE) |
| 3 | var | `thread_id` UTF-8 |
| | 2 | `message_id_len` (u16 BE) |
| | var | `message_id` UTF-8 |
| | 2 | `sender_contact_id_len` (u16 BE) |
| | var | `sender_contact_id` UTF-8 |
| | 8 | `sender_seq` (u64 BE) |
| | 4 | `session_epoch` (u32 BE) |
| | 8 | `timestamp` (i64 BE) |

**Rules:**

- `sender_seq` must match the outer signed envelope and local `ThreadMessage` for `relay_visible` rows.
- Decrypt with wrong AAD → MUST fail (no silent ignore).
- Local-only rows (`relay_visible=false`) are not encrypted for relay.

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

## Relay envelope integration (target — phase c2)

Outer envelope stays JSON + Ed25519 signature (classical). Extensions from [chat-storage DESIGN](../chat-storage-and-memory/DESIGN.md):

```json
{
  "thread_id": "uuid",
  "message_id": "uuid",
  "sender_relay_id": "relay:…",
  "sender_contact_id": "contact:…",
  "sender_seq": 42,
  "session_epoch": 1,
  "body": {
    "e2e": {
      "payload_b64": "…"
    }
  },
  "timestamp": 1234567890,
  "signature": "…"
}
```

| Channel | `body` shape | Signature covers |
|---------|--------------|------------------|
| `public_relay` | `{ "content": { …ChatPayload… } }` | message_id, thread_id, timestamp, sender_contact_id, … |
| `e2e` | `{ "e2e": { "payload_b64": "…" } }` | + `sender_seq`, `session_epoch` |

**Send pipeline (e2e):**

1. Build `ChatPayload` JSON from `ThreadMessage`.
2. Assign `(message_id, sender_seq)` at first local persist (chat-storage D010).
3. Build canonical AAD from envelope + message fields.
4. `MessageCipher::Encrypt(utf8(payload_json), session_key, aad)` → blob → base64 → `body.e2e.payload_b64`.
5. Sign outer envelope with Ed25519 identity key.
6. Relay; on receive, verify signature → decrypt → parse JSON → ingest per D013.

## Replay protection

Two layers:

1. **Cryptographic:** `sender_seq` in AAD — reusing ciphertext from another message fails decrypt or ingest.
2. **Protocol:** `ReplayWindow` in `base/crypto` + full ingest state machine in feature layer (chat-storage D013).

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
    "contact:c1|channel:e2e": {
      "master_psk_b64": "…",
      "session_epoch": 1,
      "fingerprint": "a1b2-c3d4-e5f6-…"
    }
  }
}
```

### Future: chat-target sidecar (chat-storage)

`next_outgoing_seq` may live in thread metadata or sidecar keyed by `(contact_id, channel)` — owned by chat-storage project; crypto module only reads `session_epoch` for derivation.

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

Aligned with [chat-storage D011](../chat-storage-and-memory/DECISIONS.md):

1. Ingest detects compromise (seq conflict, floor violation, etc.).
2. UI notifies user; **manual** new PSK exchange (or confirm both sides rotate).
3. `session_epoch++` on both peers; `BumpEpoch()` in store.
4. Optional `epoch_start` system message (chat-storage) as first sequenced row in new epoch.
5. HKDF uses new epoch; old epoch keys retained for decrypting historical messages locally.

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
| v2b — channel split | Required before c2 (e2e thread + `FindOrCreateDirectThread(contact, e2e)`) |
| v6 — `sender_seq`, `session_epoch` on envelope | Required before c2 |
| v6 — strict ingest D013 | Required for production E2E trust |
| v2b — “Secure message” UI | Requires c3 key import |

E2E crypto **c1** can proceed in parallel (no messaging types changed).

## Test vectors (required before c1 exit)

Frozen vector in unit tests and this design (fill at implementation):

- `master_psk`, `contact_id`, `channel`, `session_epoch` → expected `session_key` (hex)
- One AEAD tuple: `session_key`, `nonce`, `aad` (hex), `plaintext`, `ciphertext` (hex)
- One full blob round-trip: binary → base64 → binary

## Explicit non-goals

- Chaos-based or custom ciphers
- libsodium replacing BoringSSL for TLS/libp2p
- Group E2E / MLS in c1–c3
- Encrypting `identity.json` private keys (separate track)
- Forward secrecy without epoch rotation
