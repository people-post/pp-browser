# Phased roadmap

Check boxes when the work is **merged and verified**. **Design must be solid before c1** — resolve open questions in [DECISIONS.md](DECISIONS.md) and complete d0 exit criteria in [CURRENT_STATE.md](CURRENT_STATE.md).

---

## Phase d0 — Design baseline

**Goal:** Agreed spec in this project folder; no production code yet.

### Documents

- [x] [README.md](README.md) — scope, links, progress table
- [x] [DESIGN.md](DESIGN.md) — threat model, algorithms, wire format, module map, PQ posture
- [x] [DECISIONS.md](DECISIONS.md) — E001–E008 recorded from planning discussions
- [x] [CURRENT_STATE.md](CURRENT_STATE.md) — inventory + d0 checklist
- [x] [PHASES.md](PHASES.md) — this file
- [x] Cross-link from [projects/README.md](../README.md)
- [x] Cross-link from [chat-storage-and-memory/README.md](../chat-storage-and-memory/README.md)

### Design closure (required before c1)

- [x] Resolve **O001** — `body.e2e.payload_b64` (E009)
- [x] Resolve **O002** — JSON `ChatPayload` plaintext (E010)
- [x] Resolve **O003** — paste base64 (E011); rotation bundle (E020/D086)
- [x] Resolve **O006** — peer signing keys (E016)
- [x] Document canonical **Ed25519 sign payload** field list for e2e envelopes (coordinate with chat-storage v6) — E014
- [x] Add **Ed25519 frozen test vectors** (hex) to DESIGN.md § Test vectors
- [x] Add **HKDF** frozen test vector to DESIGN.md § Test vectors (E015)
- [x] Add AEAD / blob codec frozen test vectors ([`tools/gen_aead_vectors.py`](tools/gen_aead_vectors.py))
- [x] Promote to [docs/MESSAGE_ENCRYPTION.md](../../docs/MESSAGE_ENCRYPTION.md) (stable reference copy)

**Exit criteria:** All open decisions for c1 resolved; **all** frozen test vectors (incl. AEAD/codec) in DESIGN.md; `docs/MESSAGE_ENCRYPTION.md` promoted; human sign-off to start implementation.

---

## Phase c1 — `base/crypto` groundwork

**Goal:** Self-contained libsodium module; unit tests; **no** `ThreadTypes` / `P2pMessagingService` changes.

**Depends on:** d0 exit.

### Vendor libsodium

- [ ] Add `libsodium` to [scripts/vendor_import.sh](../../scripts/vendor_import.sh) (e.g. `jedisct1/libsodium` tag `1.0.20`)
- [ ] [third_party/UPSTREAM.json](../../third_party/UPSTREAM.json) + [third_party/README.md](../../third_party/README.md)
- [ ] [cmake/dependencies.cmake](../../cmake/dependencies.cmake) — `add_subdirectory`, disable tests/benchmarks
- [ ] Link `pp_base` to `sodium` in [src/base/CMakeLists.txt](../../src/base/CMakeLists.txt)

### Module `src/base/crypto/`

- [ ] `CryptoTypes.h`, `CryptoConstants.h`
- [ ] `PskFingerprint` — BLAKE2b-256 + display format
- [ ] `SessionKeyDeriver` — HKDF-SHA256 per DESIGN
- [ ] `CanonicalAad` — build/parse per byte layout
- [ ] `MessageCipher` — XChaCha20-Poly1305 AEAD
- [ ] `EncryptedPayload` — blob codec + base64
- [ ] `ReplayWindow` — seq acceptance helper
- [ ] `IPskSessionStore` (`base/crypto`) + `SqlitePskSessionStore` (`feature/messaging/`) — `chat_targets` PSK columns in `profile.db` (E008/D084, E018)

### Tests

- [ ] [src/base/crypto/tests/](../../src/base/crypto/tests/) — GTest registered from [tests/CMakeLists.txt](../../tests/CMakeLists.txt)
- [ ] HKDF determinism matches DESIGN test vector
- [ ] AEAD round-trip, tamper fail, wrong AAD fail
- [ ] Codec round-trip; ReplayWindow accept/reject
- [ ] PskSessionStore load/save round-trip; retired PSK lookup by epoch (E018)

### Docs / agent guide

- [ ] [AGENTS.md](../../AGENTS.md) — row for `base/crypto` + `docs/MESSAGE_ENCRYPTION.md`

**Exit criteria:** All c1 tests green; module usable from tests/examples without messaging includes.

---

## Phase c2 — Messaging integration

**Goal:** E2E channel encrypts body on send, decrypts on poll; public relay unchanged.

**Depends on:** c1; [chat-storage v2b](../chat-storage-and-memory/PHASES.md) (channel split); [chat-storage v6](../chat-storage-and-memory/PHASES.md) (`sender_seq`, `session_epoch` on envelope).

### Envelope and types

- [ ] `RelayMessageBody`: `body.e2e.payload_b64` + `body.content` (ChatPayload) per E009/E010
- [ ] `P2pMessagingService`: branch on `channel == e2e`
- [ ] Encrypt: serialize `ChatPayload` JSON → AAD → `MessageCipher` → `body.e2e.payload_b64`
- [ ] Decrypt on poll → parse JSON → `ThreadMessage`
- [ ] `EnvelopeSigner` — build/verify canonical sign bytes per E014 (coordinate `ChatPayloadCodec` for body hash)
- [ ] `PeerSigningKeyStore` + directory `signing_public_key_b64` on hits (E016); lazy `GET /v1/users/{relay_user_id}`
- [ ] Inbound Ed25519 verify before decrypt — resolve key from store (E016, D081)

### Ingest

- [ ] Wire `ReplayWindow` into receive path
- [ ] Fail closed on decrypt error (do not store plaintext garbage)

### Docs

- [ ] Update [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md) relay envelope section
- [ ] Update chat-storage [CURRENT_STATE.md](../chat-storage-and-memory/CURRENT_STATE.md)

**Exit criteria:** Two devices with shared PSK exchange E2E messages via relay; relay cannot read body; public thread still plaintext.

---

## Phase c3 — Key distribution UX

**Goal:** Users can establish and rotate PSK without raw developer workflows.

**Depends on:** c2; chat-storage “Secure message” thread creation (v2b).

- [ ] Settings or contact flow: import/generate PSK (raw base64 E011; bundle E020 on rotation)
- [ ] Display fingerprint; compare/confirm UI
- [ ] Add-contact: display signing-key fingerprint (E016); **`[post-v1]`** explicit confirm
- [ ] Key rotation flow: `rotate_psk` → export **`pp-browser-psk-bundle-v1`** (E020/D086); append `retired_psks[]`, bump `session_epoch` (E018); epoch-only bump (D014) without retired entry
- [ ] Bundle import: merge retired tail, cap at **`kMaxRetiredPskEpochs` (8)**; truncate disclosure when export omits older epochs
- [ ] Compromise path hooks chat-storage D011/D038 UX (choice sheet + E018 disclosure copy)
- [ ] Optional: QR encode/decode for PSK

**Exit criteria:** User can start e2e thread, verify fingerprint, send/receive, rotate after simulated epoch bump.

---

## Phase c4 — Post-quantum migration (deferred)

**Goal:** Hybrid classical + PQ for automated keying and/or signatures where EC is used.

- [ ] Evaluate liboqs vs OpenSSL OQS provider vs BoringSSL PQ APIs
- [ ] Hybrid KEM (X25519 + ML-KEM-768) for optional automated PSK setup (E013)
- [ ] Hybrid or PQ signatures for relay envelope (ML-DSA)
- [ ] Migration: dual-verify during transition window

**Exit criteria:** TBD when threat model and library support firm up.

---

## Explicitly out of scope (all phases unless new decision)

- Chaos-based encryption
- Replacing BoringSSL with libsodium globally
- Group E2E / MLS (E012 — out of scope)
- libp2p direct transport crypto rewrite
- Encrypting `identity.json` at rest (separate project)

## Suggested implementation order (cross-project)

```
d0 (this project, design)
  → c1 (base/crypto)
  → chat-storage v2b (channel split)  ─┐
  → chat-storage v6 (seq + envelope)  ─┼→ c2 (wire-up)
  → c3 (UX)                             ─┘
  → c4 (PQ, later)
```
