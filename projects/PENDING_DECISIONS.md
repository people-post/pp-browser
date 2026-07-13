# Pending decisions — human checklist

**Purpose:** Decisions to resolve before or during rollout of later implementation phases.  
**Not ADRs:** When you choose, record the outcome in the relevant [DECISIONS.md](chat-storage-and-memory/DECISIONS.md) (or e2e) and check the box here.

**Last reviewed:** 2026-07-06  
**Context:** [chat-storage CURRENT_STATE](chat-storage-and-memory/CURRENT_STATE.md) — Waves **1–7** landed (Bucket B); release hygiene pending.

---

## Priority order

| # | Decision | Status |
|---|----------|--------|
| 1 | [Release scope bucket](#1-release-scope-bucket) | ☑ **B** — [D092](chat-storage-and-memory/DECISIONS.md#d092--release-scope-bucket-b) |
| 2 | [Relay backend for history sync (D027)](#2-relay-backend-for-history-sync-d027) | ☑ **A** — relay ready — [D093](chat-storage-and-memory/DECISIONS.md#d093--relay-backend-for-v6-sync-d027) |
| 3 | [libp2p peer-direct history (D060)](#3-libp2p-peer-direct-history-d060) | ☑ **B** — required for v1 — [D094](chat-storage-and-memory/DECISIONS.md#d094--peer-direct-history-required-for-v1-d060) |
| 4 | [Group wire shape (O008)](#4-group-wire-shape-o008) | ☑ **N ciphertexts per message** — [D095](chat-storage-and-memory/DECISIONS.md#d095--group-pairwise-wire-shape-o008) |

**All checklist items resolved 2026-07-06.** Platform safety limits (O001–O004) are tracked in [platform-safety-limits/](platform-safety-limits/) — out of scope for this chat checklist.

---

## 1. Release scope bucket

**Question:** What is “done” for the first customer release?

| Option | Includes | Excludes (unless you expand) |
|--------|----------|------------------------------|
| **A — v1 private E2E** *(docs default)* | chat v2a–v6 + e2e **c1–c3** (private `e2e` tier only) | post-v4/6b/c/d, `e2e_public` auto-key, group, PQ |
| **B — v1 + post-v1 polish** | A + post-v4, post-v6b/c/d | public tier, group |
| **C — full three-tier product** | B + `e2e_public` (c3+), group (**O008**) | PQ unless added |
| **D — PQ expansion** | C + e2e **c4** (hybrid KEM / ML-DSA) | — |

**Resolved 2026-07-06:** **B — v1 + post-v1 polish** ([D092](chat-storage-and-memory/DECISIONS.md#d092--release-scope-bucket-b)). [README release scope](chat-storage-and-memory/README.md#release-scope-v1-batch) updated.

---

## 2. Relay backend for history sync (D027)

**Question:** How will v6-sync (`FetchChatTargetMessages`) be validated end-to-end?

**Re-evaluated 2026-07-06:** External relay **D027 is ready**. Client side is shipped:

- `IRelayClient::FetchChatHistory` — `HttpRelayClient` + `MockRelayClient` in `src/base/net/ServiceClientsImpl.*`
- Signed **`POST …/v1/streams/messages/query`** per [WIRE_SCHEMAS § Stream history](../docs/WIRE_SCHEMAS.md#stream-history-http-relay)
- Tests: `relay_history_test`, `chat_sync_test`

**Resolved:** **A — External relay implements D027** — coordinate integration tests against live relay for v6-sync exit ([D093](chat-storage-and-memory/DECISIONS.md#d093--relay-backend-for-v6-sync-d027)). Mock remains for CI/dev when `base_url` unset.

---

## 3. libp2p peer-direct history (D060)

**Question:** Is peer-direct **`/pp-browser/chat-history/1.0.0`** required for v1 “Sync with peer” (D059), or is relay fallback enough?

**Resolved 2026-07-06:** **B — Peer-direct required for v1** ([D094](chat-storage-and-memory/DECISIONS.md#d094--peer-direct-history-required-for-v1-d060)). Wave **v6-libp2p** (4d) is on the release critical path; relay remains fallback when libp2p is down.

---

## 4. Group wire shape (O008)

**Question:** Wire encoding for group E2E fan-out (policy: pairwise sender-keys — E022).

| Option | Implication |
|--------|-------------|
| **N ciphertexts per message** | One AEAD ciphertext per member in the outbound envelope; reuse 1:1 crypto per pair |
| Sender-keys tree | Tree-structured key distribution (deferred) |
| Encrypted fan-out via relay | Relay-side fan-out (deferred) |

**Resolved 2026-07-06:** **N ciphertexts per message** ([D095](chat-storage-and-memory/DECISIONS.md#d095--group-pairwise-wire-shape-o008)). Applies when group E2E ships (post-v1 unless scope expands to bucket C).

---

## Already decided — confirm only if you want to change

These are recorded ADRs; agents should not re-litigate. Change only with explicit product decision + new DECISIONS entry.

| Topic | Decision | ADR |
|-------|----------|-----|
| Private PSK UX | Generate/import + **mandatory fingerprint confirm** before send | E011 |
| Multi-device | **Unsupported v1** — seq conflicts → integrity pause | D015 |
| PSK at rest | **Encrypted under profile DEK** | E008 / [at-rest A005](at-rest-crypto/DECISIONS.md#a005--supersedes-e008-deferred-at-rest-for-psk) |
| Signing key trust | Relay directory + cache; fingerprint **display-only** v1 | E016 |
| Strict vs relaxed ingest | **`e2e` strict**; **`e2e_public`/group relaxed when shipped | D013, D046 |
| Scroll backfill | **Deferred** post-v1 | D052 |
| Protocol/crypto for v1 private path | **Closed** (O001–O007 resolved in e2e) | e2e DECISIONS |
| Group fan-out wire | **N ciphertexts per message** | D095 (O008) |

**Change any of the above?** ☐ No ☐ Yes — which: _______________

---

## Deferred — no decision needed for v1 batch

Documented as `[future]` or `[post-v1]` in DESIGN/PHASES:

- Scroll-triggered history backfill (post-v6c / D052)
- Per-message transport badges (post-v6d)
- Shared `@ai+` / `@ai++` (post-v6b)
- Rich ChatPayload beyond text/system (post-v4) — **in scope via bucket B**
- `sender_instance_id` / multi-device protocol (D074)
- Cross-thread FTS search
- Blockchain attestation for signing keys (D091 — post-v1)
- Display-order escape hatches (D077 — accepted complexity budget for v1)
- PQ library choice (c4 — liboqs vs OpenSSL OQS vs BoringSSL PQ)
- Platform safety limits (O001–O004) — [platform-safety-limits/](platform-safety-limits/)

---

## Changelog

| Date | Change |
|------|--------|
| 2026-07-06 | Resolved items 1–4: scope **B**, relay **ready (A)**, peer-direct **required (B)**, O008 **N ciphertexts**; removed platform-limits from checklist |
| 2026-07-02 | Created from pre-rollout decision review (human checklist) |
