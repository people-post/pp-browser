# Decisions log

Record significant choices here so future sessions (human or agent) do not re-litigate them. Format: **ID**, **date**, **decision**, **rationale**, **alternatives considered**.

---

## E001 — Symmetric E2E with manual 256-bit PSK (not chaos, not raw XOR)

**Date:** 2026-06-29  
**Decision:** E2E message bodies use a **256-bit pre-shared key** distributed out-of-band; per-message keys derived via **HKDF-SHA256**; payload encrypted with **XChaCha20-Poly1305** (AEAD).  
**Rationale:** Audited primitives, libsodium-friendly API, 256-bit symmetric margin is acceptable post-quantum (Grover); manual PSK avoids classical public-key agreement for v1.  
**Alternatives:** Chaos-based ciphers; AES-GCM without strict nonce discipline; ECDH-only key agreement without PQ hybrid.

---

## E002 — libsodium for application symmetric crypto; keep BoringSSL for TLS/libp2p

**Date:** 2026-06-29  
**Decision:** Vendor **libsodium** for `base/crypto/` (AEAD, HKDF, BLAKE2b fingerprints, CSPRNG). **Do not** replace BoringSSL/OpenSSL — curl HTTPS, libp2p TLS/SECIO/RSA/ECDSA, and existing `Ed25519Signer` remain on the OpenSSL stack until separately migrated.  
**Rationale:** libsodium is not a TLS/PKI replacement; complementary roles reduce footguns for symmetric crypto while avoiding libp2p fork rewrites.  
**Alternatives:** BoringSSL EVP for all crypto; libsodium-only monolith.

---

## E003 — Groundwork module before messaging schema changes

**Date:** 2026-06-29  
**Decision:** Phase **c1** delivers `src/base/crypto/` + unit tests + frozen test vectors **without** changing `ThreadTypes`, `RelayEnvelope`, or `P2pMessagingService`. Messaging wiring waits for **c2** and [chat-storage-and-memory](../chat-storage-and-memory/) channel split (v2b) and envelope extensions (v6).  
**Rationale:** Crypto API and wire format can be reviewed independently; avoids half-integrated relay changes.  
**Alternatives:** Big-bang PR touching envelope + P2P + crypto together.

---

## E004 — Canonical AAD binds `ChatTargetKey` context and `sender_seq`

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — LenUtf8 string fields (D088).  
**Decision:** AEAD associated data (`aad_version = 1` only): `channel`, `peer_contact_id`, `message_id`, `sender_contact_id`, `sender_seq` (u64 BE), `session_epoch` (u32 BE), `timestamp` (i64 BE) — layout in DESIGN.md. Identity/id strings are **LenUtf8** (u64 BE + UTF-8). AAD string fields carry **communicating identity values** (D079) — not local `Contact.id`, not `local:self`. Sender sets `peer_contact_id` to recipient's identity value from `ChatTargetKey`. **No `thread_id` in AAD.** No dual AAD version support (D016).  
**Rationale:** Aligns with [chat-storage D079](../chat-storage-and-memory/DECISIONS.md#d079--local-contact-vs-communicating-identity-identity-keyed-chattargetkey); local thread and address-book ids are device-private.  
**Alternatives:** Bind `thread_id` in AAD (superseded); encrypt only `text` with nonce.

---

## E005 — Ed25519 relay signing is classical; symmetric layer is PQ-safe

**Date:** 2026-06-29  
**Decision:** Treat **E2E confidentiality** (PSK + XChaCha20-Poly1305) as **post-quantum adequate** with 256-bit keys. Treat **Ed25519** envelope/identity signatures as **classical** — plan hybrid PQ upgrade (ML-DSA / ML-KEM) in phase **c4**, not a blocker for c1–c3.  
**Rationale:** Shor breaks EC signatures/key agreement, not 256-bit symmetric keys; manual PSK OOB does not create harvest-now-decrypt-later on agreement keys. Relay plaintext channel is unchanged by PQ anyway.  
**Alternatives:** Delay all E2E until PQ libraries integrated; replace Ed25519 immediately in c1.

---

## E006 — Chat target key matches chat-storage identity + channel (D079)

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — `(peer_identity_kind, peer_identity_value, channel)` (D079); HKDF `info` per E015.  
**Decision:** PSK sessions are keyed by **`ChatTargetKey` = `(peer_identity_kind, peer_identity_value, channel)`** with `channel` ∈ `{public_relay, e2e}`. Only **`e2e`** channels use message-body encryption. `session_epoch` scopes keys and seq per [chat-storage D014](../chat-storage-and-memory/DECISIONS.md). HKDF `info` uses **`channel` + `epoch` only** (E015) — not identity strings.  
**Rationale:** Same identity boundary as thread model D004/D079; public relay stays signed plaintext; pair scoping is the per-target `master_psk`.  
**Alternatives:** Per-`local_thread_id` keys only; one PSK for all contacts; identity in HKDF `info` (rejected — asymmetric derivation per peer view).

---

## E007 — Encrypted wire blob: nested `body.e2e` + base64 (E009)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — nested object (E009).  
**Decision:** E2E relay `body` shape is **`{ "e2e": { "payload_b64": "…" } }`**. `payload_b64` decodes to **`[version:1][nonce:24][ciphertext+tag]`**. Outer envelope fields remain signed JSON. Public channel uses **`{ "content_b64": "…" }`** over binary `ChatPayload` (D087).  
**Rationale:** Extensible nested shape; separates public structured content from E2E blob.  
**Alternatives:** Flat `body.ciphertext_b64`; encrypt entire envelope JSON.

---

## E009 — Nested `body.e2e` object for ciphertext

**Date:** 2026-06-29  
**Decision:** Ciphertext field is **`body.e2e.payload_b64`**, not a top-level `body` string. Public relay uses sibling **`body.content_b64`** for binary `ChatPayload` (D087).  
**Rationale:** Room for future `body.e2e.key_id` or algorithm hints without breaking public messages.  
**Alternatives:** `body.ciphertext_b64`; `body.e2e.ciphertext`.

---

## E010 — AEAD plaintext is binary `ChatPayload`

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — binary layout (D087); supersedes JSON plaintext.  
**Decision:** Bytes encrypted inside the E2E blob are **binary `ChatPayload` v1** — the same bytes as public **`body.content_b64`** after base64 decode — see [chat-storage D087](../chat-storage-and-memory/DECISIONS.md#d087--binary-chatpayload-v1-e014-body_hash--e010-plaintext) / [WIRE_SCHEMAS](../chat-storage-and-memory/WIRE_SCHEMAS.md#chatpayload-v1--binary-d087). Not raw `text` only. Max decrypted size **`kMaxE2ePlaintextBytes` (128 KiB)** per [chat-storage D029](../chat-storage-and-memory/DECISIONS.md); reject before `ChatPayloadCodec::Decode` after decrypt.  
**Rationale:** Contact cards, annotations, and crypto txs work identically on E2E and public; one codec path; no JSON canonicalization drift between sign and encrypt.  
**Alternatives:** UTF-8 `text` only; JSON `ChatPayload` (rejected — D087).

---

## E008 — PSK store v1 in `profile.db` `chat_targets`; at-rest encryption deferred

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — superseded `sessions.json`; columns on `chat_targets` (D084).  
**Decision:** PSK material lives in **`profile.db` → `chat_targets`** alongside `session_epoch` and `next_outgoing_seq` — keyed by **`ChatTargetKey`** PK (D047/D084). Columns: `master_psk_b64`, `psk_fingerprint`, `retired_psks_json` (`e2e` channel only; `NULL` on `public_relay`). **`IPskSessionStore`** in `base/crypto` is the seam; v1 default impl **`SqlitePskSessionStore`** in `feature/messaging/` reads/writes those columns under the **`profile.db` writer mutex** (same txn as epoch bump). No `profiles/{id}/crypto/sessions.json`. At-rest risk class matches today's `encrypted_private_key_b64` misnomer in `IdentityStore`; OS keychain backend is follow-up, not c1 blocker.  
**Rationale:** Crypto session is per chat target, not per thread shell; colocating PSK with seq/epoch avoids cross-file races on epoch bump and survives delete/recreate of `local_thread_id`.  
**Alternatives:** `sessions.json` sidecar (rejected — dual-store sync); PSK in `thread.db` (rejected — shell is ephemeral); block c1 on keychain integration.

---

## E011 — PSK entry UX v1: paste base64

**Date:** 2026-07-02  
**Updated:** 2026-07-02 — initial setup; rotation uses E020 bundle.  
**Decision:** Phase **c3** PSK import accepts:

1. **Initial setup (epoch 1):** paste **raw base64** — 32-byte key, standard base64 (44 chars, optional `=` padding). App decodes, stores via **`IPskSessionStore`** into **`chat_targets.master_psk_b64`** (+ `psk_fingerprint`), `session_epoch = 1` (or leaves epoch unchanged when reinstalling same target).
2. **`rotate_psk` / multi-hop catch-up:** paste **`pp-browser-psk-bundle-v1`** JSON ([E020](#e020--rich-oob-psk-bundle-v1)) — active key + bounded retired tail (D086).

Display **BLAKE2b fingerprint** of the **active** `master_psk` for out-of-band verification with the peer.  
**Rationale:** Minimal UI for first contact; rich bundle fixes O006 without a second wire protocol.  
**Alternatives:** Paste fingerprint + confirm (no raw key paste); QR scan (deferred UX — bundle-friendly).

---

## E012 — Group E2E out of scope

**Date:** 2026-07-02  
**Decision:** **Group / multi-party E2E** is **out of scope** for all current phases (c1–c4). Direct `(contact_id, channel=e2e)` only. No shared group PSK, no MLS, no sender-keys scheme in this project unless a future decision reopens scope.  
**Rationale:** v1 targets 1:1 chat aligned with `ChatTargetKey`; group crypto is a separate product and protocol surface.  
**Alternatives:** Shared group PSK (weak membership model); MLS (large protocol + UX lift).

---

## E013 — Optional hybrid KEM for automated PSK setup in c4

**Date:** 2026-07-02  
**Decision:** Phase **c4** adds **optional automated key agreement** via hybrid **X25519 + ML-KEM-768**; shared secret feeds HKDF as `master_psk` input (same salt/info labels as manual PSK). **Manual OOB paste** (E011) remains supported — users choose either path per contact. Classical-only KEM (X25519 or ECDH alone) is **not** permitted.  
**Rationale:** Improves UX for new E2E contacts without weakening PQ posture on agreement; on-wire AEAD format (E007/E010) unchanged; aligns c4 PQ library work with relay signature upgrade (E005).  
**Alternatives:** Manual PSK only forever (simpler, no liboqs dependency).

---

## E014 — Canonical Ed25519 relay envelope signing bytes

**Date:** 2026-07-02  
**Decision:** Relay **`RelayEnvelope`** signatures use **fixed binary signing bytes** (not JSON). Layout in [DESIGN.md § Ed25519 signing](DESIGN.md#ed25519-canonical-signing-bytes). Summary:

- **Domain prefix:** `"pp-browser:relay-envelope-sign-v1\0"` (UTF-8 + NUL), then **`sign_version = 1`** byte, then fields below.
- **Signed fields:** `envelope_version`, `route_kind`, `channel`, `timestamp` (Unix **milliseconds**, i64 BE), `sender_seq` (u64 BE), `session_epoch` (u32 BE), **`body_hash`** (BLAKE2b-256, 32 bytes), `message_id`, `sender_contact_id` (length-prefixed UTF-8 strings, u16 BE length).
- **E2E seq/epoch on public:** wire omits `sender_seq`/`session_epoch` on `public_relay` (D045); signing uses **`0`** for both.
- **Body hash:** `BLAKE2b-256(body_kind || payload_bytes)` — `body_kind=0x01` + **decoded** **`body.content_b64`** bytes for `public_relay`; `body_kind=0x02` + **decoded** `body.e2e.payload_b64` bytes for `e2e`. Same hash function; branch-specific payload extraction.
- **Not signed:** `thread_id`, `sender_relay_id`, `signature`, unknown top-level keys (D073).
- **Signature wire encoding:** standard **base64** (RFC 4648, padded) only in v1 — matches `Ed25519Signer`.
- **Implementation:** shared **`EnvelopeSigner`** in `src/base/messaging/`; c1 test vectors and c2 wiring must use it.

**Rationale:** JSON `dump()` signing is non-canonical and today's code incorrectly includes `thread_id`. Binary layout matches `CanonicalAad` style; BLAKE2b aligns with PSK fingerprints (E002); body hash binds public binary body (D087/D078) and E2E ciphertext bytes separately.  
**Alternatives:** Sign canonical JSON of outer envelope (rejected — key order/whitespace footguns); SHA-256 body hash (acceptable but splits hash primitive from libsodium story); hash base64 string for E2E (rejected — binds encoding not ciphertext); JSON `ChatPayload` body hash (rejected — D087).

---

## E015 — HKDF `info`: channel + epoch only (Option A)

**Date:** 2026-07-02  
**Decision:** Session key derivation uses:

```
info = "channel:{channel}|epoch:{session_epoch}"
```

with `ikm = master_psk` and `salt = "pp-browser-msg-v1"` unchanged. **Do not** include `peer_identity_kind`, `peer_identity_value`, or local `Contact.id` in HKDF `info`.  
**Rationale:** `master_psk` is already unique per **`ChatTargetKey`** (one OOB secret per peer identity + channel). Including the peer identity in `info` would differ per device (`alice→bob` vs `bob→alice`) and produce **different session keys** for the same conversation. Channel + epoch provide epoch rotation without breaking cross-peer symmetry.  
**Alternatives:** Sorted canonical pair of both parties' identities in `info` (acceptable but redundant with per-target PSK); identity in `info` (rejected — interoperability bug).

---

## E016 — Peer signing keys: relay directory source, local cache, OOB fingerprint at add

**Date:** 2026-07-02  
**Decision:** Inbound **`EnvelopeSigner::Verify`** (receive pipeline step 2) resolves the sender's **Ed25519 public key** from a local **`PeerSigningKeyStore`** keyed by **`(peer_identity_kind, peer_identity_value)`** — the same communicating-identity boundary as `ChatTargetKey` (D079). **PSK** (E001) and **signing keys** are independent trust anchors.

| Layer | v1 policy |
|-------|-----------|
| **Source of truth (relay)** | Directory exposes **`signing_public_key_b64`** (32-byte Ed25519, RFC 4648 base64) per **`relay_user`** id — on search hits and via **`GET /v1/users/{relay_user_id}`** lazy lookup. Relay already receives `public_key` at registration. |
| **Persist at add-contact** | **`AddFromDirectoryHit`** (and manual add flows) write the key into **`PeerSigningKeyStore`** when directory supplies it. **`DirectoryHit`** gains optional **`signing_public_key_b64`** on the primary `relay_user` hit. |
| **OOB verification** | On add, display **BLAKE2b-256 fingerprint** of the decoded public key (same grouped-hex style as PSK — E011). **`[v1]`** display-only; **`[post-v1]`** optional explicit fingerprint-confirm step before trust. |
| **Lazy fetch** | If ingest needs a key not cached (e.g. ephemeral public preview — D080): **`GET /v1/users/{relay_user_id}`**, cache in store, then verify. **Fail closed** if lookup fails. |
| **Rotation** | Relay updates directory mapping when user re-registers with a new key. Client may refresh on verify failure. **New `relay_user` id** → new communicating identity → **new thread** (D079); same identity + new key → update store, no new thread. |
| **Rejected** | Embed key or fingerprint in **`sender_relay_id`** / **`sender_contact_id`**; **`ContactIdKind::SigningKey`** mixed into `ids[]`; TOFU pin on first message without directory or OOB; infer full public key from `relay:` + base64 prefix (today's local `substr(0,12)` bootstrap is **not** reversible). |

**On-disk (v1):** `{data_dir}/profiles/{profile_id}/crypto/signing_keys.json` — map key `identity:{kind}:{value}` → `{ "signing_public_key_b64": "…", "fingerprint": "…" }`.

**Rationale:** `sender_contact_id` on the wire is a **routing identity**, not a public key (E014, D079). Production ingest cannot verify without an explicit binding. Relay directory is the natural registry; local cache avoids per-message HTTP; OOB fingerprint matches PSK UX; lazy fetch supports D080 ephemeral public without pre-added contacts.  
**Alternatives considered:** Directory-only verify with no local cache (rejected — offline, hot-path latency); paste-only keys with no directory (rejected — poor UX for search-driven add); encode key in relay id (rejected — wrong layer, rotation pain).

---

## E017 — Relay-user identity value format

**Date:** 2026-07-02  
**Cross-project:** [chat-storage D082](../chat-storage-and-memory/DECISIONS.md#d082--relay-user-communicating-identity-string-format).  
**Decision:** When **`peer_identity_kind = relay_user`**, the identity **value** string is **`relay:<opaque_id>`** — relay-assigned, URL-safe, not derived from the signing public key. Same string in envelope **`sender_contact_id`**, AAD, signing bytes, directory hits, and **`identity.json`** after registration. **v1:** **`sender_relay_id`** matches **`sender_contact_id`**.

**Frozen test fixture:** `relay:alice123` (E014 vectors in DESIGN.md).

**Rejected:** `relay:user:<id>` (draft nomenclature); pubkey-prefix bootstrap on wire (E016).

**Rationale:** Exact UTF-8 byte match across peers is required for AAD and signature verify; one canonical format avoids silent interoperability failure.  
**Alternatives:** See D082.

---

## E018 — Retired PSK ledger for historical decrypt after `rotate_psk`

**Date:** 2026-07-02  
**Cross-project:** [chat-storage D083](../chat-storage-and-memory/DECISIONS.md#d083--retired-psk-ledger-on-rotate_psk-e018), [D084](../chat-storage-and-memory/DECISIONS.md#d084--psk-columns-on-chat_targets-in-profiledb-e008).  
**Decision:** When **`rotate_psk`** replaces `master_psk` and bumps `session_epoch`, append the **previous** `(epoch, master_psk_b64)` to **`chat_targets.retired_psks_json`** **before** writing the new active PSK (same `profile.db` txn — E008/D084). Decrypt resolves `master_psk` by `envelope.session_epoch`: active PSK when `epoch == session_epoch`, else lookup retired array for a matching `epoch`, then HKDF (E015).

| Bump kind | `retired_psks_json` |
|-----------|---------------------|
| **`rotate_psk`** (new `master_psk` + `session_epoch++`) | Append `{ "epoch": <old>, "master_psk_b64": "…", "retired_at": <unix_ms> }` |
| **Epoch-only** ([chat-storage D014](../chat-storage-and-memory/DECISIONS.md#d014--peer-reset-requires-session_epoch-bump) — same `master_psk`, `session_epoch++`) | **No** entry — re-derive any past epoch from current `master_psk` |

**Pruning (v1):** Drop a retired entry for epoch `E` when **all** hold: (1) no local `messages` rows with `session_epoch = E` for that chat target, (2) user ran **Clear messages** for that epoch surface or explicitly abandons old-epoch sync, (3) no active old-epoch gap/sync work. Pruning is optional hygiene — correctness does not require immediate purge. **Hard cap:** ledger MUST NOT exceed **`kMaxRetiredPskEpochs` (8)** — prune **lowest** epoch entries first (D086/E020).

**UX disclosure (c3):** On **Start new secure chat**, choice sheet states: messages **already saved on this device** stay readable (plaintext per D048); relay ciphertext from **before** rotation remains decryptable on this device via retained retired PSKs; **new** traffic uses the new PSK and epoch only; other devices need the new PSK for the new epoch.

**Rejected for v1:** Permanent loss of pre-rotation relay ciphertext without amending DESIGN; full re-encrypt of local history at rotation (local bodies are plaintext — D048/D069; heavy relay backfill does not replace a small ledger); unbounded key history without pruning policy.

**Rationale:** DESIGN § Key rotation promises old-epoch decrypt locally; a single `master_psk_b64` per target breaks that on PSK rotation. Retired ledger is minimal, one entry per rotation event, and matches the threat model (local disk already holds plaintext transcripts). Epoch-only bumps need no ledger because HKDF re-derives from the unchanged `master_psk`.  
**Alternatives:** Store derived `session_key` per epoch instead of retired `master_psk` (acceptable optimization, deferred); re-encrypt-at-rotation (wrong layer for plaintext storage).

---

## E019 — Decrypt and HKDF use `envelope.session_epoch`

**Date:** 2026-07-02  
**Cross-project:** [chat-storage D085](../chat-storage-and-memory/DECISIONS.md#d085--passive-epoch-advance-peer-bumps-first).  
**Decision:** On **inbound** E2E decrypt, **`SessionKeyDeriver`** and **`MessageCipher::Decrypt`** MUST use **`envelope.session_epoch`** for:

1. **`IPskSessionStore::ResolveMasterPskForEpoch(envelope.session_epoch)`** — active `master_psk_b64` when `envelope.session_epoch == chat_targets.session_epoch`, else `retired_psks_json` lookup (E018); **never** pass `chat_targets.session_epoch` when it lags the envelope on passive adopt.
2. **HKDF `info`** — `"pp-browser-msg-v1" + channel + envelope.session_epoch` (E015).

**Outbound encrypt** uses **`chat_targets.session_epoch`** (authoritative send counter). After **passive adopt** (D085), step 12 persist updates `chat_targets.session_epoch` in the same transaction as the inbound row so the next outbound send matches the adopted epoch.

**Rationale:** Using the cached local epoch for HKDF while the envelope carries a higher epoch would either fail decrypt (wrong key) or, if mis-implemented, create a split-brain where inbound decrypt succeeds at epoch N while `next_outgoing_seq` still advances in epoch N−1.  
**Alternatives:** Decrypt with `max(envelope, local)` without persisting adopt (rejected — leaves outbound stale); lazy adopt on first outbound send (rejected — D085).

---

## E020 — Rich OOB PSK bundle v1

**Date:** 2026-07-02  
**Cross-project:** [chat-storage D086](../chat-storage-and-memory/DECISIONS.md#d086--rich-oob-psk-bundle-with-bounded-retired-epochs-o006), [D029 `kMaxRetiredPskEpochs`](../chat-storage-and-memory/DECISIONS.md#d029--chat-resource-bounds-size--volume).  
**Decision:** **`rotate_psk`** OOB export/import uses JSON format **`pp-browser-psk-bundle-v1`**:

```json
{
  "format": "pp-browser-psk-bundle-v1",
  "channel": "e2e",
  "active_epoch": 3,
  "master_psk_b64": "<32-byte key, RFC 4648 base64>",
  "retired_epochs": [
    { "epoch": 1, "master_psk_b64": "…" },
    { "epoch": 2, "master_psk_b64": "…" }
  ]
}
```

| Field | Rules |
|-------|-------|
| `format` | Must be `pp-browser-psk-bundle-v1` |
| `channel` | Must be `e2e` |
| `active_epoch` | uint32 ≥ 1 |
| `master_psk_b64` | Decodes to 32 bytes — key for **`active_epoch`** (live send/recv) |
| `retired_epochs` | Optional array; max **`kMaxRetiredPskEpochs` (8)** entries; each `epoch` < `active_epoch`; strictly increasing epochs; no duplicates |

**Export (initiator after `rotate_psk`):** include `active_epoch` + new `master_psk_b64` + retired tail — up to **8** most recent epochs in `(active_epoch - K .. active_epoch - 1]` from local `retired_psks_json` plus the epoch just retired. Serialized bundle ≤ **`kMaxPskBundleBytes` (4 KiB)**.

**Import (innocent peer):** validate → merge `retired_epochs` into `chat_targets.retired_psks_json` (dedupe by epoch) → set active PSK + **`session_epoch = active_epoch`** → reset `next_outgoing_seq = 1` → cancel old-epoch pending/outbox (D068/D086) — one **`profile.db` txn**. Show active fingerprint (E011). If export was truncated (peer rotated more than K times offline), disclose that relay ciphertext **outside the retired tail** may not decrypt on this device.

**Initial contact:** raw base64 (E011) equivalent to bundle with `active_epoch: 1`, empty `retired_epochs[]`.

**Rationale:** Resolves O006 — innocent peer learns skipped intermediate PSKs without a wire ack; bounded tail matches paste/QR constraints and on-disk ledger cap.  
**Alternatives:** Unbounded chain (rejected — OOB size); round-trip rotation gate only (rejected — O006-A); single-key paste on rotation (rejected — multi-hop gap).
