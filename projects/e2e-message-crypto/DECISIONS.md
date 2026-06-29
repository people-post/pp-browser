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

## E004 — Canonical AAD binds context and `sender_seq`

**Date:** 2026-06-29  
**Decision:** AEAD associated data includes: protocol version, `thread_id`, `message_id`, `sender_contact_id`, `sender_seq` (u64 BE), `session_epoch` (u32 BE), `timestamp` (i64 BE) — byte layout fixed in DESIGN.md. `sender_seq` prevents cut-and-paste across messages within an epoch.  
**Rationale:** Aligns with [chat-storage D008–D011](../chat-storage-and-memory/DECISIONS.md); ciphertext cannot be replayed with altered ordering without detection.  
**Alternatives:** Encrypt only `text` with nonce; rely on outer Ed25519 for seq binding only.

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
**Alternatives:** Per-`thread_id` keys only; one PSK for all contacts.

---

## E007 — Encrypted wire blob is versioned binary + base64 in JSON relay

**Date:** 2026-06-29  
**Decision:** On the wire, encrypted body is **`[version:1][nonce:24][ciphertext+tag]`** encoded as base64 in the relay JSON field (name TBD — see open questions). Outer envelope fields (`thread_id`, `message_id`, `sender_seq`, `session_epoch`, `timestamp`) remain in signed JSON; inner plaintext is UTF-8 message text for v1.  
**Rationale:** Compact, unambiguous parsing; JSON relay unchanged except body field.  
**Alternatives:** NaCl-style ASCII armor only; encrypt entire envelope JSON.

---

## E008 — PSK store v1 is profile-scoped JSON; at-rest encryption deferred

**Date:** 2026-06-29  
**Decision:** `JsonPskSessionStore` at `{data_dir}/profiles/{id}/crypto/sessions.json` with base64 PSK — same risk class as today's `encrypted_private_key_b64` misnomer in `IdentityStore`. Document limitation; OS keychain backend is a follow-up, not c1 blocker.  
**Rationale:** Ship correct crypto on the wire first; local secret storage hardening is orthogonal seam (`IPskSessionStore`).  
**Alternatives:** Block c1 on keychain integration.

---

## Open decisions (not yet resolved)

| ID | Question | Options |
|----|----------|---------|
| O001 | Ciphertext JSON field name | `body.ciphertext_b64`; `body.e2e.payload_b64`; nested object |
| O002 | AEAD plaintext v1 | UTF-8 `text` only; JSON `{"text","content_rml?"}` |
| O003 | PSK entry UX v1 | Paste base64; paste fingerprint + confirm; QR (later) |
| O004 | Automated key agreement | Manual PSK only forever; optional hybrid KEM in c4 |
| O005 | Group E2E | Out of scope; shared group PSK; MLS (far future) |

When resolved, move rows to numbered decisions above.
