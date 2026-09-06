# Decisions log

Record significant choices here so future sessions (human or agent) do not re-litigate them. Format: **ID**, **date**, **decision**, **rationale**, **alternatives considered**.

---

## E001 — Symmetric E2E with 256-bit PSK (not chaos, not raw XOR)

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — all product P2P tiers E2E (E021); manual OOB distribution is **private direct** only.  
**Decision:** E2E message bodies use a **256-bit pre-shared key**; per-message keys derived via **HKDF-SHA256**; payload encrypted with **XChaCha20-Poly1305** (AEAD). **Private direct:** PSK distributed out-of-band (E011). **Public direct / group:** automated or pairwise keying (E013/E022).  
**Rationale:** Audited primitives, libsodium-friendly API, 256-bit symmetric margin is acceptable post-quantum (Grover); manual PSK avoids classical public-key agreement for v1.  
**Alternatives:** Chaos-based ciphers; AES-GCM without strict nonce discipline; ECDH-only key agreement without PQ hybrid.

---

## E002 — libsodium for application symmetric crypto; keep BoringSSL for TLS/libp2p

**Date:** 2026-06-29  
**Decision:** Vendor **libsodium** for `foundation/crypto/` (AEAD, HKDF, BLAKE2b fingerprints, CSPRNG). **Do not** replace BoringSSL/OpenSSL — curl HTTPS, libp2p TLS/SECIO/RSA/ECDSA, and existing `Ed25519Signer` remain on the OpenSSL stack until separately migrated.  
**Rationale:** libsodium is not a TLS/PKI replacement; complementary roles reduce footguns for symmetric crypto while avoiding libp2p fork rewrites.  
**Alternatives:** BoringSSL EVP for all crypto; libsodium-only monolith.

---

## E003 — Groundwork module before messaging schema changes

**Date:** 2026-06-29  
**Decision:** Phase **c1** delivers `src/foundation/crypto/` + unit tests + frozen test vectors **without** changing `ThreadTypes`, `RelayEnvelope`, or `MeshDeliveryOrchestrator`. Messaging wiring waits for **c2** and [chat-storage-and-memory](../chat-storage-and-memory/) channel split (v2b) and envelope extensions (v6).  
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
**Rationale:** Shor breaks EC signatures/key agreement, not 256-bit symmetric keys; manual PSK OOB does not create harvest-now-decrypt-later on agreement keys. All product P2P tiers use E2E bodies on the wire (D089/E021).  
**Alternatives:** Delay all E2E until PQ libraries integrated; replace Ed25519 immediately in c1.

---

## E006 — Chat target key matches chat-storage identity + channel (D079)

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — three tiers (D089/E021); both direct E2E channels encrypt.  
**Decision:** PSK sessions are keyed by **`ChatTargetKey` = `(peer_identity_kind, peer_identity_value, channel)`** with direct `channel` ∈ **`{ e2e, e2e_public }` only** (D090). Both use message-body encryption; HKDF `info` includes the wire `channel` string (E015). `session_epoch` scopes keys and seq per [chat-storage D014](../chat-storage-and-memory/DECISIONS.md).  
**Rationale:** Same identity boundary as thread model D004/D079; separate PSK per tier even for the same peer.  
**Alternatives:** Per-`local_thread_id` keys only; one PSK for all contacts; plaintext public direct (rejected — E021).

---

## E007 — Encrypted wire blob: nested `body.e2e` + base64 (E009)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — nested object (E009).  
**Decision:** E2E relay `body` shape is **`{ "e2e": { "payload_b64": "…" } }`** only on direct (D090). `payload_b64` decodes to **`[version:1][nonce:24][ciphertext+tag]`**.  
**Rationale:** Extensible nested shape; separates public structured content from E2E blob.  
**Alternatives:** Flat `body.ciphertext_b64`; encrypt entire envelope JSON.

---

## E009 — Nested `body.e2e` object for ciphertext

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — optional **`key_init_b64`** for `e2e_public` auto-key (E024).  
**Decision:** Ciphertext field is **`body.e2e.payload_b64`**, not a top-level `body` string. Both direct tiers use this shape (D090). Optional **`body.e2e.key_init_b64`** on **`e2e_public`** carries hybrid KEM encapsulation when the recipient may lack `master_psk` (E024) — relay may forward; not included in E014 `body_hash`.  
**Rationale:** Room for `key_id` / KEM hints without breaking public messages; separates ciphertext from key-establishment blob.  
**Alternatives:** `body.ciphertext_b64`; `body.e2e.ciphertext`; top-level `key_init` (rejected — keep E2E fields nested).

---

## E010 — AEAD plaintext is binary `ChatPayload`

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — binary layout (D087); supersedes JSON plaintext.  
**Decision:** Bytes encrypted inside the E2E blob are **binary `ChatPayload` v1** — see [chat-storage D087](../chat-storage-and-memory/DECISIONS.md#d087--binary-chatpayload-v1-e014-body_hash--e010-plaintext) / [WIRE_SCHEMAS](../../docs/contracts/WIRE_SCHEMAS.md#chatpayload-v1--binary-d087d088). Not raw `text` only. Max decrypted size **`kMaxE2ePlaintextBytes` (128 KiB)** per [chat-storage D029](../chat-storage-and-memory/DECISIONS.md); reject before `ChatPayloadCodec::Decode` after decrypt.  
**Rationale:** Contact cards, annotations, and crypto txs work identically on E2E and public; one codec path; no JSON canonicalization drift between sign and encrypt.  
**Alternatives:** UTF-8 `text` only; JSON `ChatPayload` (rejected — D087).

---

## E008 — PSK store v1 in `profile.db` `chat_targets`; at-rest via profile DEK

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — superseded `sessions.json`; columns on `chat_targets` (D084).  
**Updated:** 2026-07-11 — at-rest encryption shipped ([at-rest A005](../at-rest-crypto/DECISIONS.md#a005--supersedes-e008-deferred-at-rest-for-psk)); PSK + retired ledger blobs encrypted under profile DEK; fingerprints remain plaintext.  
**Decision:** PSK material lives in **`profile.db` → `chat_targets`** for **`e2e`** and **`e2e_public`** channels (D090). On disk, `master_psk_b64` and retired `master_psk_b64` entries are **AEAD ciphertext** (base64) under the profile DEK from `vault.bin` — see [AT_REST_ENCRYPTION.md](../../docs/contracts/AT_REST_ENCRYPTION.md).  
**Rationale:** Crypto session is per chat target, not per thread shell; colocating PSK with seq/epoch avoids cross-file races on epoch bump and survives delete/recreate of `local_thread_id`.  
**Alternatives:** `sessions.json` sidecar (rejected — dual-store sync); PSK in `thread.db` (rejected — shell is ephemeral); block c1 on keychain integration.

---

## E011 — PSK establishment UX: private manual; public automated (E021)

**Date:** 2026-07-02  
**Updated:** 2026-07-02 — three tiers (E021); manual flow is **private direct (`e2e`)** only.  
**Decision:** Phase **c3** PSK establishment for **private direct (`e2e`)**:

1. **Generate (either peer):** 32 bytes from CSPRNG (`randombytes_buf`). **Either peer may generate** — cryptographically equivalent; both peers MUST hold identical bytes (E015). **UX default:** device starting **Secure message** offers **Generate new key**; peer uses **Import**. Import-only path when peer already generated elsewhere.
2. **Export (epoch 1):** show **raw RFC 4648 base64** (44 chars, optional `=` padding) + **Copy** + BLAKE2b fingerprint of active `master_psk` — same encoding as import. QR deferred.
3. **Import:** paste raw base64 → decode → store via **`IPskSessionStore`** into **`chat_targets.master_psk_b64`** (+ `psk_fingerprint`), `session_epoch = 1` (or leave epoch unchanged when reinstalling same target). Clear **`psk_verified_at`** on import.
4. **`rotate_psk` / multi-hop catch-up:** initiator **exports**; innocent peer **imports** **`pp-browser-psk-bundle-v1`** JSON ([E020](#e020--rich-oob-psk-bundle-v1)) — active key + bounded retired tail (D086). Clear **`psk_verified_at`** on bundle import; show active fingerprint.
5. **Verify before first send:** user MUST compare fingerprint OOB with peer, then explicitly confirm (**"I've verified this fingerprint with my contact"**). E2E compose/send disabled until PSK installed **and** confirmed. Persist **`psk_verified_at`** (unix ms) on **`chat_targets`**; clear on PSK replace, import, or **`rotate_psk`**. Fingerprint display alone is insufficient (contrast E016 signing keys — directory-backed, display-only in v1).

**Rationale:** Symmetric shared PSK; minimal OOB surface for private tier; explicit confirm closes TOFU gap where PSK has no directory source of truth. **`e2e_public`** uses automated establishment (E021/E013/E024).  
**Alternatives:** Initiator-only generation (rejected — unnecessary constraint); JSON bundle for epoch 1 (rejected — heavier than needed); send without verify step on private tier (rejected); same manual flow for public tier (rejected — E021).

---

## E012 — Group E2E: pairwise sender-keys (supersedes out-of-scope)

**Date:** 2026-07-02  
**Updated:** 2026-07-02 — reopened for UX-first group tier with pairwise keys (E022, D089).  
**Decision:** **Group E2E** is in product (after c1–c3). It uses: **pairwise sender-keys** (encrypt per member using pair-wise secrets), **not** a single shared group PSK and **not** MLS in the first slice. Ingest policy matches **`e2e_public`** (relaxed default — D046). Wire shape: **N ciphertexts per message** ([chat-storage D095](../chat-storage-and-memory/DECISIONS.md#d095--group-pairwise-wire-shape-o008)).  
**Rationale:** Single group PSK is weak on membership change; MLS is heavy UX; pairwise model reuses 1:1 crypto machinery and matches user preference for pair keys.  
**Alternatives:** Shared group PSK (rejected); MLS (deferred); group out of scope entirely (superseded — E022).

---

## E013 — Hybrid KEM for automated PSK setup (`e2e_public`, c4)

**Date:** 2026-07-02  
**Updated:** 2026-07-02 — O007 resolved in [E024](#e024--auto-key-trust-anchor-for-e2e_public-o007); KEM is the **only** PSK establishment path for `e2e_public`.  
**Decision:** Phase **c4** (library) / **c3+** (public tier feature) adds **automated key agreement** via hybrid **X25519 + ML-KEM-768** for **`e2e_public`** (and optionally group pair-key bootstrap). Shared secret feeds HKDF as `master_psk` input — see [E024 § PSK derivation](DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007). **Manual OOB** (E011) remains required for **`e2e` (private direct)**. Classical-only KEM (X25519 or ECDH alone) is **not** permitted.  
**Rationale:** Public direct tier targets fluency — auto key init without weakening PQ posture on agreement. Private tier keeps manual OOB.  
**Alternatives:** Manual PSK only forever (rejected for `e2e_public` — E021); directory-sealed PSK without KEM (rejected — E024/O007).

---

## E014 — Canonical Ed25519 relay envelope signing bytes

**Date:** 2026-07-02  
**Decision:** Relay **`RelayEnvelope`** signatures use **fixed binary signing bytes** (not JSON). Layout in [DESIGN.md § Ed25519 signing](DESIGN.md#ed25519-canonical-signing-bytes). Summary:

- **Domain prefix:** `"pp-browser:relay-envelope-sign-v1\0"` (UTF-8 + NUL), then **`sign_version = 1`** byte, then fields below.
- **Signed fields:** `envelope_version`, `route_kind`, `channel`, `timestamp` (Unix **milliseconds**, i64 BE), `sender_seq` (u64 BE), `session_epoch` (u32 BE), **`body_hash`** (BLAKE2b-256, 32 bytes), `message_id`, `sender_contact_id` (length-prefixed UTF-8 strings, u16 BE length).
- **Direct channel enum (sign bytes):** `0` = `e2e`, `1` = `e2e_public` (D090). Reject `public_relay`.
- **Body hash:** `BLAKE2b-256(0x02 ‖ decoded E2E blob bytes)` only — no `0x01` plaintext path.
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

with `ikm = master_psk` and `salt = "pp-browser-msg-v1"` unchanged. **`channel`** is the wire string **`e2e`** or **`e2e_public`** (E021). **Do not** include `peer_identity_kind`, `peer_identity_value`, or local `Contact.id` in HKDF `info`.  
**Rationale:** `master_psk` is already unique per **`ChatTargetKey`** (one OOB secret per peer identity + channel). Including the peer identity in `info` would differ per device (`alice→bob` vs `bob→alice`) and produce **different session keys** for the same conversation. Channel + epoch provide epoch rotation without breaking cross-peer symmetry.  
**Alternatives:** Sorted canonical pair of both parties' identities in `info` (acceptable but redundant with per-target PSK); identity in `info` (rejected — interoperability bug).

---

## E016 — Peer signing keys: relay directory source, local cache, OOB fingerprint at add

**Date:** 2026-07-02  
**Updated:** 2026-07-02 — lookup via **`IPeerSigningKeyResolver`** (E024); relay directory is v1 backend, not hardcoded in ingest.  
**Decision:** Inbound **`EnvelopeSigner::Verify`** (receive pipeline step 2) resolves the sender's **Ed25519 public key** via **`IPeerSigningKeyResolver`** → local **`PeerSigningKeyStore`** keyed by **`(peer_identity_kind, peer_identity_value)`** — the same communicating-identity boundary as `ChatTargetKey` (D079). **PSK** (E001) and **signing keys** are independent trust anchors — see [E024](DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007).

| Layer | v1 policy |
|-------|-----------|
| **Source of truth (relay)** | Directory exposes **`signing_public_key_b64`** (32-byte Ed25519, RFC 4648 base64) per **`relay_user`** id — on search hits and via **`GET /v1/users/{relay_user_id}`** lazy lookup. Relay already receives `public_key` at registration. |
| **Persist at add-contact** | **`AddFromDirectoryHit`** (and manual add flows) write the key into **`PeerSigningKeyStore`** when directory supplies it. **`DirectoryHit`** gains optional **`signing_public_key_b64`** on the primary `relay_user` hit. |
| **OOB verification** | On add, display **BLAKE2b-256 fingerprint** of the decoded public key (same grouped-hex style as PSK — E011). Display-only; **`[later]`** optional explicit fingerprint-confirm step before trust. |
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
| `channel` | Must be `e2e` or `e2e_public` |
| `active_epoch` | uint32 ≥ 1 |
| `master_psk_b64` | Decodes to 32 bytes — key for **`active_epoch`** (live send/recv) |
| `retired_epochs` | Optional array; max **`kMaxRetiredPskEpochs` (8)** entries; each `epoch` < `active_epoch`; strictly increasing epochs; no duplicates |

**Export (initiator after `rotate_psk`):** include `active_epoch` + new `master_psk_b64` + retired tail — up to **8** most recent epochs in `(active_epoch - K.. active_epoch - 1]` from local `retired_psks_json` plus the epoch just retired. Serialized bundle ≤ **`kMaxPskBundleBytes` (4 KiB)**.

**Import (innocent peer):** validate → merge `retired_epochs` into `chat_targets.retired_psks_json` (dedupe by epoch) → set active PSK + **`session_epoch = active_epoch`** → reset `next_outgoing_seq = 1` → cancel old-epoch pending/outbox (D068/D086) — one **`profile.db` txn**. Show active fingerprint (E011). If export was truncated (peer rotated more than K times offline), disclose that relay ciphertext **outside the retired tail** may not decrypt on this device.

**Initial contact:** raw base64 (E011) equivalent to bundle with `active_epoch: 1`, empty `retired_epochs[]`.

**Rationale:** Resolves O006 — innocent peer learns skipped intermediate PSKs without a wire ack; bounded tail matches paste/QR constraints and on-disk ledger cap.  
**Alternatives:** Unbounded chain (rejected — OOB size); round-trip rotation gate only (rejected — O006-A); single-key paste on rotation (rejected — multi-hop gap).

---

## E021 — Three chat tiers; both direct tiers E2E (D089)

**Date:** 2026-07-02  
**Cross-project:** [chat-storage D089](../chat-storage-and-memory/DECISIONS.md#d089--three-chat-tiers-both-direct-tiers-e2e-e021).  
**Decision:** Product P2P chat has **three E2E tiers**:

| Tier | Wire `channel` (direct) | Key establishment | Priority |
|------|-------------------------|-------------------|----------|
| **Private direct** | `e2e` | Manual OOB PSK + mandatory fingerprint (E011) | Security first |
| **Public direct** | `e2e_public` | Hybrid KEM PSK (E013/E024) + signing resolver (E016) | UX first |
| **Group** | `route.kind=group` | Pairwise sender-keys (E022) | UX first |

**Both direct tiers** encrypt bodies with the same AEAD stack (E001/E010). **No `public_relay`** (D090/E023).

**Engineering posture:** UX-first tiers accept policy tradeoffs (relaxed ingest D046, auto rotation tuned for history recovery, multi-device target) while still using AEAD, signed envelopes, pinned signing keys (E016), and seq in AAD.

**Phasing:** c1–c3 target **private direct (`e2e`)** only. **`e2e_public`** after c3 + auto-key path. **Group** (E022).

**Rationale:** “Public” chat must not mean relay-readable plaintext; users still get a strict tier for high-assurance contacts.  
**Alternatives:** Plaintext direct wire (rejected — D090); single tier with security slider (rejected).

---

## E022 — Group E2E: pairwise sender-keys

**Date:** 2026-07-02  
**Cross-project:** [chat-storage D076/D089](../chat-storage-and-memory/DECISIONS.md#d076--group-chat-placeholders-in-catalog--sync-scope).  
**Decision:** Group message bodies are E2E encrypted using **pairwise sender-keys** — the sender encrypts for each member using pair-wise secrets (reuse 1:1 crypto machinery where possible), **not** a single shared group PSK. MLS is deferred. Membership changes rotate affected pair keys. Ingest policy: relaxed default (D046). Wire shape: **N ciphertexts per message** — [chat-storage D095](../chat-storage-and-memory/DECISIONS.md#d095--group-pairwise-wire-shape-o008) (O008 resolved).

**Rationale:** User preference for pair keys over group-wide secret; avoids weak membership model of shared PSK; reuses `ChatTargetKey`-style pair scoping.  
**Alternatives:** Single group PSK (rejected); MLS (deferred); plaintext group relay (rejected — E021).

---

## E023 — No `public_relay` wire value (D090)

**Date:** 2026-07-02  
**Cross-project:** [chat-storage D090](../chat-storage-and-memory/DECISIONS.md#d090--no-public_relay--plaintext-direct-wire).  
**Decision:** **`public_relay`**, **`body.content_b64`**, and **`body_kind=0x01`** body hashing are **removed** from the protocol. Direct envelopes: **`e2e`** \| **`e2e_public`**, **`body.e2e.payload_b64`**, **`body_kind=0x02`** only. AAD/sign channel enum: **`0`** = `e2e`, **`1`** = `e2e_public`. Regenerate frozen Ed25519/AEAD test vectors after enum change.  
**Rationale:** Greenfield cutover (D016); no transitional plaintext path.  
**Alternatives:** Bootstrap shim (rejected).

---

## E024 — Auto-key trust anchor for `e2e_public` (O007)

**Date:** 2026-07-02  
**Updated:** 2026-08-13 — encapsulate-to is the **account** ML-KEM-768 (**M015**); private `e2e` stays device-local.  
**Updated:** 2026-08-15 — public `rotate_psk` policy: account-scope chats do not auto-rotate; device-lock is [E027](#e027--public-11-device-lock-rekey-auto-rotate_psk-only-when-both-sides-are-device-bound).  
**Cross-project:** [chat-storage D080](../chat-storage-and-memory/DECISIONS.md#d080--inbound-routing-private-find-only-public-auto-create), [D081](../chat-storage-and-memory/DECISIONS.md#d081--peer-signing-key-lookup-before-envelope-verify-e016), [D091](../chat-storage-and-memory/DECISIONS.md#d091--blockchain-contact-id-caip-10-e024), [multi-device M015](../multi-device-account/DECISIONS.md#m015--account-kem-for-publicgroup-auto-key-private-e2e-stays-device-local).  
**Decision:** Resolve **O007**. **`e2e_public`** auto-key uses **two independent trust anchors**. Neither anchor may be the relay **learning or choosing `master_psk`**.

### Anchor 1 — Signing (who sent the envelope)

| Rule | Detail |
|------|--------|
| **API** | **`IPeerSigningKeyResolver::Resolve(kind, identity_value)`** → `{ signing_public_key_b64, fingerprint, source, source_ref?, trusted_at? }` |
| **Cache** | **`PeerSigningKeyStore`** — same key as E016; persist resolver results with **provenance** (`source`: `relay_directory`, `manual_paste`, `on_chain`, …; `source_ref`: tx hash / registry id when applicable) |
| **v1 backends** | **`RelayDirectoryResolver`** — directory search hit + lazy **`GET /v1/users/{relay_user_id}`** (E016/D081); **`ManualPasteResolver`** — user paste at add-contact |
| **`[later]` backend** | **`OnChainAttestationResolver`** — verify on-chain attestation (CAIP-10 → Peer ID / communicating identity — D091/D096) binding `(peer_identity_kind, peer_identity_value)` → `signing_public_key_b64` |
| **v1 ingest policy** | **Relay directory** — fail closed if key missing or verify fails (D081) |
| **`[later]` ingest policy** | **Chain-preferred** — when a valid on-chain attestation exists for the communicating identity, it **confirms or overrides** the relay key; relay-only binding accepted when no chain attestation is present |
| **Rejected** | Relay as sole long-term trust with no upgrade path; TOFU pin on first message without directory; key embedded in `sender_contact_id` (E016) |

Receive pipeline step 2 calls the resolver — **do not** hardcode relay HTTP in the ingest path.

### Anchor 2 — PSK (message confidentiality)

| Rule | Detail |
|------|--------|
| **Mechanism** | **Account ML-KEM-768 only** (E026 / **M015**): encapsulate to the **person**, not a device. Directory `kem_public_key_b64` is the account public key; linked devices share the secret |
| **PSK derivation** | `master_psk = HKDF-SHA256(ikm = kem_shared_secret, salt = "pp-browser-msg-v1", info = "auto-key-v1|channel:e2e_public")` — 32-byte output |
| **Session keys** | Unchanged (E015): `info = "channel:e2e_public|epoch:{session_epoch}"` from `master_psk`. Account-scope public does **not** auto-`rotate_psk` (**E027**); epoch-only bumps remain for reset |
| **KEM public keys** | Each **account** publishes ML-KEM-768 via register / `GET /v1/users` — relay stores **public** keys only. Link-device copies the account KEM secret (**M015**). Private (`e2e`) does **not** use this handshake |
| **Wire carry** | Optional **`body.e2e.key_init_b64`** on `e2e_public` envelopes when the recipient may not yet hold `master_psk` (first message / auto-create path). Relay may store and forward this blob; it MUST NOT decrypt or replace it |
| **Receive step 7** | **`AutoKeyEstablishment::DeriveMasterPsk(envelope)`** — decapsulate `key_init_b64` with the **local account KEM** secret when local `master_psk` missing; else **`IPskSessionStore::ResolveMasterPskForEpoch`** (E018) |
| **Rejected** | Directory-sealed PSK; relay-generated or relay-held shared secret; relay-assisted PSK distribution where relay learns `master_psk`; classical-only KEM (E013); per-device KEM + N `key_init`s |

### Inbound auto-create participant gate (D080)

Unchanged from D080 — independent of PSK anchor:

| Check | Rule |
|-------|------|
| Envelope signature | Must pass step 2 |
| Signing key | Resolver must return a key for `(peer_identity_kind, envelope.sender_contact_id)` |
| Blocklist | **`[future]`** — `TrustLevel::Blocked` on linked Contact (not part of O007) |
| v1 default | Accept any **cryptographically verified** sender with resolvable signing key |

Optional **`psk_verified_at`** on `e2e_public` remains deferred (D089) — no mandatory OOB PSK fingerprint before first send.

### Blockchain hookup (lookup + attestation — not wire identity v1)

- **`ContactIdKind::Blockchain`** values use **CAIP-10** ([D091](../chat-storage-and-memory/DECISIONS.md#d091--blockchain-contact-id-caip-10-e024)) — e.g. `eip155:1:0x…`
- **Role (D096):** CAIP-10 is a first-class **lookup** handle — resolve to **libp2p Peer ID** (+ signing pubkey). It is not the product network id and not v1 wire identity.
- On-chain attestation (future) links **CAIP-10 account** ↔ **Peer ID** ↔ **Ed25519 signing key** (optional relay id) — strengthens Anchor 1 only
- **`ChatTargetKey` / wire `sender_contact_id`** remain **`relay_user`** until a deliberate protocol bump to `peer_id`; blockchain does **not** replace hybrid KEM for PSK

### Phasing

| Work | Phase |
|------|-------|
| `IPeerSigningKeyResolver` + relay backend + ingest step 2 | **c2** |
| Private `e2e` manual PSK (E011) | **c3** |
| `e2e_public` auto-key + `key_init_b64` + KEM libs | **c3+** (feature); KEM library integration may track **c4** (E013) |

**Rationale:** Splitting anchors lets blockchain verify **identity ↔ signing key** without making the relay a PSK broker. Hybrid KEM keeps confidentiality even against a curious relay. CAIP-10 prepares search/attestation without forcing a wire identity migration.  
**Alternatives rejected:** Relay-sealed PSK (relay learns secret); relay as combined identity+PSK anchor; manual OOB for public tier (E021); blockchain address on wire in v1 (premature).

---

## E025 — Account envelope signing; private PSK not auto-synced

**Date:** 2026-08-11  
**Updated:** 2026-08-13 — public auto-key encapsulate-to is account KEM (**M015**).  
**Amends:** E016/E024 verify target (account key once Account ID is on wire); E011 private PSK distribution remains OOB — **not** fan-out on link-device.  
**Canonical:** [multi-device-account](../multi-device-account/) M003, M005, M008, **M015** ([DESIGN](../multi-device-account/DESIGN.md)).  
**Cross-project:** [chat-storage D099](../chat-storage-and-memory/DECISIONS.md#d099--account-id-amends-d096-multi-device), [at-rest A010](../at-rest-crypto/DECISIONS.md#a010--shared-dek-per-device-vault-wrap-multi-device).

**Decision:**

1. **Account ML-DSA-65 signs** all relay envelopes (`mldsa-native`). Device ML-DSA-65 is for Peer ID / libp2p Noise identity only ([libp2p-pq-transport P004](../libp2p-pq-transport/DECISIONS.md#p004--hard-cut--wipe-amend-m003m008e025)).
2. Friends verify using the **ML-DSA-65** account public key bound to Account ID (hash-binding M002; directory/cache — resolver seam as E016, key kind shifts with D099/m2).
3. **Private (`e2e`) PSKs are not auto-synced** to linked devices. New device needs OOB/import or explicit opt-in; default link-device does **not** copy private `chat_targets` PSK material.
4. **Public (`e2e_public`) / group** conversation PSKs **may** sync with account/DEK when those tiers + link-device ship. Directory auto-key encapsulate-to is the **account KEM** (copied on link — **M015** / E024).
5. Message **body** encryption remains PSK AEAD on all tiers — device-bound private means **which installs hold the PSK**, not a different cipher.

**Rationale:** One person-level PQ verify path; private tier keeps higher assurance under multi-device account keys.  
**Alternatives:** Device-signed envelopes (S2/S3); sync all PSKs with DEK; Ed25519+ML-DSA hybrid.

---

## E026 — ML-KEM-768 via mlkem-native (replaces X25519+Kyber-draft)

**Date:** 2026-08-11  
**Amends:** E013/E024 hybrid KEM — **ML-KEM-768 only** (no X25519 half); library = vendored **mlkem-native** (not BoringSSL experimental Kyber).  
**Cross-project:** [multi-device-account M008](../multi-device-account/DECISIONS.md#m008--pq-libraries-and-kem-mlkem-native--mldsa-native-ml-kem-768-only), [SERVICE_ENDPOINTS](../../docs/contracts/SERVICE_ENDPOINTS.md) register KEM size **1184**.

**Decision:**

1. `HybridKem` implements **FIPS 203 ML-KEM-768** (`mlkem_keypair` / `enc` / `dec`).
2. Sizes: pk **1184**, sk **2400**, ct **1088**, ss **32**.
3. Pre-release: regenerate stored profile KEM blobs if size ≠ ML-KEM-768.
4. Brief accepts 1184-byte `kem_public_key_b64` on register/directory.

**Rationale:** Aggressive PQ for auto-key; PQCP C backend; drops draft Kyber + classical KEM half.  
**Alternatives:** Keep X25519+ML-KEM hybrid; liboqs.

---

## E027 — Public 1:1 device-lock rekey; auto-`rotate_psk` only when both sides are device-bound

**Date:** 2026-08-15  
**Status:** Accepted.  
**Amends:** [E024](#e024--auto-key-trust-anchor-for-e2e_public-o007) (public rotation policy); [E011](#e011--psk-establishment-ux-private-manual-public-automated-e021) (public rotate is in-band, not OOB); [E018](#e018--retired-psk-ledger-for-historical-decrypt-after-rotate_psk) (public lock uses the same retired ledger).  
**Cross-project:** [M020](../multi-device-account/DECISIONS.md#m020--device-scoped-public-psks-stay-off-the-link-bundle), [D101](../chat-storage-and-memory/DECISIONS.md#d101--public-key_scope-psk_rotate-ingest-and-rotation-policy), [D085](../chat-storage-and-memory/DECISIONS.md#d085--passive-epoch-advance-peer-bumps-first), [D089](../chat-storage-and-memory/DECISIONS.md#d089--three-chat-tiers-both-direct-tiers-e2e-e021).

**Decision:**

1. **Default public 1:1** stays account-KEM auto-key (**M015** / E024). No chooser on start or accept. Group and private `e2e` are unchanged. Account-scope public chats do **not** auto-`rotate_psk`. Epoch-only bumps remain for reset (D014).
2. After a public 1:1 thread exists, either side may **Use only this device…**. That is an in-band `rotate_psk`: new random `master_psk`, `session_epoch++`, old PSK retired (E018). The peer does **not** confirm; they auto-adopt (D085) and see a system line. The tap locks **the initiator’s other installs** only. Unlock / “use all devices again” is out of this slice.
3. **Wire:** `content_type=system`, `control_type=psk_rotate`, on `e2e_public` only. AEAD payload uses the **current** `master_psk` so every install that already has the key can read the notice. The **new** PSK is **not** wrapped under the old PSK. It travels in `body.e2e.key_init_b64`, encapsulated to:
 - peer **account KEM** if the peer has not published a conversation KEM (`wrap_kind=account_kem` — first lock);
 - peer **conversation KEM** once present (`wrap_kind=thread_kem` — second lock and D2D auto-rekey).
4. `detail` JSON (inside AEAD) MUST include `rotation_id` (UUID), `new_epoch` (u32), `wrap_kind` (`account_kem` | `thread_kem`), initiator `thread_kem_pk_b64` (ML-KEM-768 public, RFC 4648), and `key_init_hash` (BLAKE2b-256 of the raw `key_init` bytes, hex). A swapped `key_init` that does not match `key_init_hash` is a hard reject. E014 `body_hash` still does not cover `key_init` (E009).
5. On lock, the initiator generates a **conversation ML-KEM-768** keypair for this `ChatTargetKey`. The secret stays on this install (DEK-wrapped on disk). The public key is in `detail.thread_kem_pk_b64`. The peer stores it as `peer_thread_kem_pk` and uses it for later wraps to that person.
6. **Concurrent rotations:** both control messages use the **old** epoch. Winner = lexicographically greater `rotation_id` (UUID string, case-sensitive). Loser aborts. An inbound `psk_rotate` aborts an in-flight local lock. Two different PSKs at the same epoch = **hard crypto failure**, not public LWW (D046).
7. **Auto-`rotate_psk`** (quiet, same control type, wrap to conversation KEMs) runs only when `key_scope=device_pair` (both sides have locked). Trigger: next outbound send after **100 messages in this epoch** or **7 days** since `last_psk_rotate_at`. Not on `account` or `device_self`. Delay while a 1:1 call is active; refuse user lock while a call is active.
8. Do **not** reuse private OOB `RotatePskAndExportBundle` for this path.

**Rationale:** Account KEM keeps public chat fluent across linked devices. An explicit lock is the honest no-confirm story (each side drops only their own siblings). Wrapping a new PSK under the old PSK would give every sibling the new key. Auto-rekey before both sides publish conversation KEMs can undo a lock or brick the peer’s other devices without consent.

**Alternatives:** Chooser on start/accept (rejected — fights public fluency); one tap locks both parties’ devices (rejected — consent change for the peer); wrap new PSK under old PSK (rejected — does not exclude siblings); per-device directory KEM + N `key_init`s (rejected — M015); unlock in this slice (deferred).
