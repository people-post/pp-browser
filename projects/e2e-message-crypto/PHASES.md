# Phased roadmap

Check boxes when the work is **merged and verified**. **Design must be solid before c1** — resolve open questions in [DECISIONS.md](DECISIONS.md) and complete d0 exit criteria in [CURRENT_STATE.md](CURRENT_STATE.md).

This file has **two orderings**:

| Ordering | Use when |
|----------|----------|
| **Phase sections below** (d0 → c1 → c2 → …) | Traceability, exit criteria, dependencies |
| **[Agent batch delivery](#agent-batch-delivery-order)** | Agents finish all `[v1]` work before a single release — parallel with [chat-storage waves](../chat-storage-and-memory/PHASES.md#agent-batch-delivery-order) |

---

## Traceability

| Phase | Agent wave | Depends on | Maturity |
|-------|------------|------------|----------|
| d0 | — (done) | — | Design baseline |
| c1 | 1 | d0 | `base/crypto`, frozen vectors, no messaging types |
| c2 | 5 | c1; chat [v2b](../chat-storage-and-memory/PHASES.md) + [v6](../chat-storage-and-memory/PHASES.md) | Private `e2e` encrypt/decrypt on relay |
| c3 | 6 | c2; chat v2b “Secure message” | PSK UX, rotation, verify gate |
| c3+ | 7 | c3 | `e2e_public` auto-key (E013/E024) |
| c4 | — (deferred) | c3+ | Post-quantum hybrid |

---

## Agent batch delivery order

For **batch delivery** (agents complete work before one release), coordinate with [chat-storage agent waves](../chat-storage-and-memory/PHASES.md#agent-batch-delivery-order). Phase checklists below are unchanged.

### Critical path (cannot parallelize)

```
d0 (complete)
  → c1  ∥  chat v2a-core          ← wave 1
  → chat v2a-p2p + v2b            ← wave 2
  → chat v3 ∥ v4                  ← wave 3
  → chat v6 (split sub-packages)  ← wave 4 — blocks c2
  → c2 → c3                       ← waves 5–6
```

**c2 is blocked by chat v6** (`sender_seq`, `session_epoch` on envelope for AAD + E014). **c1 is not** — start c1 in wave 1 alongside chat v2a-core.

### E2E waves (this project)

| Wave | Phase | Work | Checkpoint |
|------|-------|------|------------|
| **1** | **c1** | Vendor libsodium; `src/base/crypto/*`; `SqlitePskSessionStore` skeleton; **all** frozen vector tests | No `#include` of `ThreadTypes` / `P2pMessagingService` in `base/crypto` |
| **5** | **c2** | `P2pMessagingService` encrypt/decrypt; `EnvelopeSigner`; `PeerSigningKeyStore`; inbound verify before decrypt | Two devices, shared PSK, relay sees ciphertext only |
| **6** | **c3** | Generate/export/import PSK; fingerprint gate; rotation bundle (D086); compromise hooks to chat D038 | User verify + send/receive + simulated epoch bump |
| **7** | **c3+** | Public tier auto-key (E013/E024) + chat `e2e_public` functional | Out of v1 batch unless scope expands |

### Agent session reading list

1. [DESIGN.md](DESIGN.md) — module map, test vectors
2. [docs/MESSAGE_ENCRYPTION.md](../../docs/MESSAGE_ENCRYPTION.md) — normative wire/crypto (agents implement against this)
3. [chat-storage WIRE_SCHEMAS.md](../chat-storage-and-memory/WIRE_SCHEMAS.md) — `RelayEnvelope`, binary ChatPayload (c2+)
4. [chat-storage DESIGN § Implementer constraints](../chat-storage-and-memory/DESIGN.md#implementer-constraints) — when touching envelope or store columns
5. Relevant **phase checklist** below + [CURRENT_STATE.md](CURRENT_STATE.md) (update in same PR)

### Rollout gates to skip in batch mode

| Gate | Batch-mode alternative |
|------|-------------------------|
| c1 before any chat work | c1 only needs d0 — **parallel** with v2a-core |
| c2 after full v6 UX | c2 needs v6 **envelope + ingest pipeline**, not every v6 UX banner |
| Per-phase doc churn | Batch [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md) / [AGENTS.md](../../AGENTS.md) updates at end of c1 or c3 |

### Anti-patterns (cause rework)

- Messaging includes from `base/crypto` before c2
- Encrypt/decrypt before chat v6 adds `sender_seq` / `session_epoch` to envelope types
- Signing JSON `dump()` instead of E014 canonical bytes
- Skipping frozen vector tests in c1 (“fix in c2”)

### Scope: what “all phases” means

| Include in v1 batch | Defer |
|---------------------|-------|
| d0, c1, c2, c3 | c4 (PQ) |
| Private `e2e` only | c3+ `e2e_public` auto-key unless explicitly in scope |
| | Group E2E (E022), identity encryption at rest |

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
- [x] Resolve **O002** — binary `ChatPayload` plaintext (E010/D087)
- [x] Resolve **O003** — PSK establishment UX (E011); rotation bundle (E020/D086)
- [x] Resolve **O006** — peer signing keys (E016)
- [x] Resolve **O007** — `e2e_public` auto-key trust anchor (E024); CAIP-10 blockchain ids (D091)
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

**Goal:** E2E direct tiers encrypt body on send, decrypt on poll (D090).

**Depends on:** c1; [chat-storage v2b](../chat-storage-and-memory/PHASES.md) (channel split); [chat-storage v6](../chat-storage-and-memory/PHASES.md) (`sender_seq`, `session_epoch` on envelope).

### Envelope and types

- [ ] `RelayMessageBody`: `body.e2e.payload_b64` only per E009/E010/D090
- [ ] `P2pMessagingService`: branch on `channel == e2e`
- [ ] Encrypt: `ChatPayloadCodec::Encode` → AAD → `MessageCipher` → `body.e2e.payload_b64`
- [ ] Decrypt on poll → `ChatPayloadCodec::Decode` → `ThreadMessage`
- [ ] `EnvelopeSigner` — build/verify canonical sign bytes per E014 (coordinate `ChatPayloadCodec` for body hash)
- [ ] `IPeerSigningKeyResolver` + `RelayDirectoryResolver` + `PeerSigningKeyStore` with provenance (E016/E024); lazy `GET /v1/users/{relay_user_id}`
- [ ] Inbound Ed25519 verify before decrypt — resolve key via resolver (E016/E024, D081)

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

- [ ] Secure-message flow: **Generate** PSK (CSPRNG, E011) — default for thread starter
- [ ] **Export** epoch-1 key: raw base64 + Copy + fingerprint display (E011)
- [ ] **Import** PSK: raw base64 (E011) or bundle on rotation (E020/D086)
- [ ] Fingerprint compare + explicit confirm; persist **`psk_verified_at`**; E2E send gate until verified (E011)
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

## Explicitly out of scope (c1–c3 unless noted)

- Chaos-based encryption
- Replacing BoringSSL with libsodium globally
- **Group E2E** — `[post-v1]` pairwise sender-keys (E022)
- **Public tier auto-key** — after c3 (E021/E013/E024)
- libp2p direct transport crypto rewrite
- Encrypting `identity.json` at rest (separate project)

## Suggested implementation order (cross-project)

See **[Agent batch delivery](#agent-batch-delivery-order)** and [chat-storage agent waves](../chat-storage-and-memory/PHASES.md#agent-batch-delivery-order) for parallel work. Dependency spine:

```
d0 (complete)
  → c1 ∥ chat v2a-core
  → chat v2a-p2p + v2b
  → chat v3 ∥ v4
  → chat v6 (schema → pipeline → sync → libp2p → integrity)
  → c2 → c3
  → c3+ / c4 (e2e_public auto-key, then PQ) — post-v1 unless in scope
  → group E2E (E022, post-v1)
```

---

## Changelog

| Date | Change |
|------|--------|
| 2026-07-02 | Agent batch delivery order — waves, reading list, cross-project critical path; traceability table |
