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
**Updated:** 2026-06-29 — `peer_contact_id` + `channel`; no `thread_id` (D056).  
**Decision:** AEAD associated data (`aad_version = 1` only): `channel`, `peer_contact_id`, `message_id`, `sender_contact_id`, `sender_seq` (u64 BE), `session_epoch` (u32 BE), `timestamp` (i64 BE) — layout in DESIGN.md. Sender sets `peer_contact_id` to the other party in the `ChatTargetKey`. **No `thread_id` in AAD.** No dual AAD version support (D016).  
**Rationale:** Aligns with [chat-storage D056](../chat-storage-and-memory/DECISIONS.md); local thread ids are device-private.  
**Alternatives:** Bind `thread_id` in AAD (superseded); encrypt only `text` with nonce.

---

## E005 — Ed25519 relay signing is classical; symmetric layer is PQ-safe

**Date:** 2026-06-29  
**Decision:** Treat **E2E confidentiality** (PSK + XChaCha20-Poly1305) as **post-quantum adequate** with 256-bit keys. Treat **Ed25519** envelope/identity signatures as **classical** — plan hybrid PQ upgrade (ML-DSA / ML-KEM) in phase **c4**, not a blocker for c1–c3.  
**Rationale:** Shor breaks EC signatures/key agreement, not 256-bit symmetric keys; manual PSK OOB does not create harvest-now-decrypt-later on agreement keys. Relay plaintext channel is unchanged by PQ anyway.  
**Alternatives:** Delay all E2E until PQ libraries integrated; replace Ed25519 immediately in c1.

---

## E006 — Chat target key matches chat-storage `(contact_id, channel)`

**Date:** 2026-06-29  
**Decision:** PSK sessions and HKDF context use **`ChatTargetKey` = `(contact_id, channel)`** with `channel` ∈ `{public_relay, e2e}`. Only **`e2e`** channels use message-body encryption. `session_epoch` scopes keys and seq per [chat-storage D014](../chat-storage-and-memory/DECISIONS.md).  
**Rationale:** Same identity boundary as thread model D004; public relay stays signed plaintext.  
**Alternatives:** Per-`local_thread_id` keys only; one PSK for all contacts.

---

## E007 — Encrypted wire blob: nested `body.e2e` + base64 (E009)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — nested object (E009).  
**Decision:** E2E relay `body` shape is **`{ "e2e": { "payload_b64": "…" } }`**. `payload_b64` decodes to **`[version:1][nonce:24][ciphertext+tag]`**. Outer envelope fields remain signed JSON. Public channel uses **`{ "content": { …ChatPayload… } }`** plaintext (D026).  
**Rationale:** Extensible nested shape; separates public structured content from E2E blob.  
**Alternatives:** Flat `body.ciphertext_b64`; encrypt entire envelope JSON.

---

## E009 — Nested `body.e2e` object for ciphertext

**Date:** 2026-06-29  
**Decision:** Ciphertext field is **`body.e2e.payload_b64`**, not a top-level `body` string. Public relay uses sibling **`body.content`** for `ChatPayload` JSON.  
**Rationale:** Room for future `body.e2e.key_id` or algorithm hints without breaking public messages.  
**Alternatives:** `body.ciphertext_b64`; `body.e2e.ciphertext`.

---

## E010 — AEAD plaintext is JSON `ChatPayload`

**Date:** 2026-06-29  
**Decision:** Bytes encrypted inside the E2E blob are **UTF-8 JSON** of the same `ChatPayload` object used on the public channel (`schema_version`, `content_type`, `text`, `payload`) — see [chat-storage D026](../chat-storage-and-memory/DECISIONS.md). Not raw `text` only. Max decrypted size **`kMaxE2ePlaintextBytes` (128 KiB)** per [chat-storage D029](../chat-storage-and-memory/DECISIONS.md); reject before JSON parse after decrypt.  
**Rationale:** Contact cards, annotations, and crypto txs work identically on E2E and public; one codec path.  
**Alternatives:** UTF-8 `text` only; separate binary framing per type.

---

## E008 — PSK store v1 is profile-scoped JSON; at-rest encryption deferred

**Date:** 2026-06-29  
**Decision:** `JsonPskSessionStore` at `{data_dir}/profiles/{id}/crypto/sessions.json` with base64 PSK — same risk class as today's `encrypted_private_key_b64` misnomer in `IdentityStore`. Document limitation; OS keychain backend is a follow-up, not c1 blocker.  
**Rationale:** Ship correct crypto on the wire first; local secret storage hardening is orthogonal seam (`IPskSessionStore`).  
**Alternatives:** Block c1 on keychain integration.

---

## E011 — PSK entry UX v1: paste base64

**Date:** 2026-07-02  
**Decision:** Phase **c3** PSK import uses **paste base64** — user pastes a 32-byte key encoded as standard base64 (44 chars, optional `=` padding). App decodes, stores in `JsonPskSessionStore`, and displays **BLAKE2b fingerprint** for out-of-band verification with the peer.  
**Rationale:** Minimal UI for c3; matches `sessions.json` storage format; fingerprint display still satisfies DESIGN.md verification step without a separate "confirm fingerprint first" import flow.  
**Alternatives:** Paste fingerprint + confirm (no raw key paste); QR scan (deferred UX).

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
- **Body hash:** `BLAKE2b-256(body_kind || payload_bytes)` — `body_kind=0x01` + canonical **`ChatPayload`** UTF-8 JSON for `public_relay`; `body_kind=0x02` + **decoded** `body.e2e.payload_b64` bytes for `e2e`. Same hash function; branch-specific payload extraction.
- **Not signed:** `thread_id`, `sender_relay_id`, `signature`, unknown top-level keys (D073).
- **Signature wire encoding:** standard **base64** (RFC 4648, padded) only in v1 — matches `Ed25519Signer`.
- **Implementation:** shared **`EnvelopeSigner`** in `src/base/messaging/`; c1 test vectors and c2 wiring must use it.

**Rationale:** JSON `dump()` signing is non-canonical and today's code incorrectly includes `thread_id`. Binary layout matches `CanonicalAad` style; BLAKE2b aligns with PSK fingerprints (E002); body hash binds public semantics (D078 JSON) and E2E ciphertext bytes separately.  
**Alternatives:** Sign canonical JSON of outer envelope (rejected — key order/whitespace footguns); SHA-256 body hash (acceptable but splits hash primitive from libsodium story); hash base64 string for E2E (rejected — binds encoding not ciphertext).
