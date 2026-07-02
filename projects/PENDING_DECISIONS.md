# Pending decisions — human checklist

**Purpose:** Decisions to resolve before or during rollout of later implementation phases.  
**Not ADRs:** When you choose, record the outcome in the relevant [DECISIONS.md](chat-storage-and-memory/DECISIONS.md) (or e2e / platform-safety-limits) and check the box here.

**Last reviewed:** 2026-07-02  
**Context:** [chat-storage CURRENT_STATE](chat-storage-and-memory/CURRENT_STATE.md) — Waves 1–2 + v3 + v6-schema + **v6-pipeline** landed; **v6-sync** in progress.

---

## Priority order

Resolve top-to-bottom. Items 1–3 affect release definition and integration testing; 4–5 can run in parallel with ongoing waves.

| # | Decision | Status |
|---|----------|--------|
| 1 | [Release scope bucket](#1-release-scope-bucket) | ☐ |
| 2 | [Relay backend for history sync (D027)](#2-relay-backend-for-history-sync-d027) | ☐ |
| 3 | [libp2p peer-direct history (D060)](#3-libp2p-peer-direct-history-d060) | ☐ |
| 4 | [Platform safety limits timing (O001–O004)](#4-platform-safety-limits-timing) | ☐ |
| 5 | [Group wire shape (O008)](#5-group-wire-shape-o008) | ☐ |

---

## 1. Release scope bucket

**Question:** What is “done” for the first customer release?

| Option | Includes | Excludes (unless you expand) |
|--------|----------|------------------------------|
| **A — v1 private E2E** *(docs default)* | chat v2a–v6 + e2e **c1–c3** (private `e2e` tier only) | post-v4/6b/c/d, `e2e_public` auto-key, group, PQ |
| **B — v1 + post-v1 polish** | A + post-v4, post-v6b/c/d | public tier, group |
| **C — full three-tier product** | B + `e2e_public` (c3+), group (**O008**) | PQ unless added |
| **D — PQ expansion** | C + e2e **c4** (hybrid KEM / ML-DSA) | — |

**References:** [PHASES § Scope buckets](chat-storage-and-memory/PHASES.md#scope-buckets), [e2e PHASES § Scope](e2e-message-crypto/PHASES.md#scope-what-all-phases-means).

**Your choice:** ☐ A ☐ B ☐ C ☐ D — notes: _______________

**When resolved:** Update [chat-storage README](chat-storage-and-memory/README.md) release scope line if not A.

---

## 2. Relay backend for history sync (D027)

**Question:** How will v6-sync (`FetchChatTargetMessages`) be validated end-to-end?

v6-sync needs relay support that is **not fully in this repo** today:

- Client: `IRelayClient::FetchChatHistory` (or equivalent) — in progress
- Server: **`GET /v1/chat-targets/messages`** per [WIRE_SCHEMAS](chat-storage-and-memory/WIRE_SCHEMAS.md) — party auth, seq-scoped fetch (D027)
- Also needed for c2 verify path: **`signing_public_key_b64`** on directory hits + lazy **`GET /v1/users/{relay_user_id}`** (E016/D081)

| Option | Implication |
|--------|-------------|
| **A — External relay implements D027 before v6-sync exit** | Coordinate relay team; client + server integration tests |
| **B — Client-first; mock relay until external relay ships** | v6-sync exit criteria use mock only; production sync blocked on relay |
| **C — In-repo relay stub/service for dev/CI** | Extra scope in pp-browser or sibling repo |

**Your choice:** ☐ A ☐ B ☐ C — owner / timeline: _______________

**When resolved:** Note in [CURRENT_STATE](chat-storage-and-memory/CURRENT_STATE.md) under messaging if mock-only is acceptable for wave 4c exit.

---

## 3. libp2p peer-direct history (D060)

**Question:** Is peer-direct **`/pp-browser/chat-history/1.0.0`** required for v1 “Sync with peer” (D059), or is relay fallback enough?

Docs default: **relay fallback satisfies v1 exit**; peer-direct preferred when libp2p is up ([PHASES wave 4d](chat-storage-and-memory/PHASES.md#agent-batch-delivery-order) — “relay-only stub OK first”). libp2p messaging glue is still a stub in-tree.

| Option | Implication |
|--------|-------------|
| **A — Relay-only for v1** | Defer v6-libp2p to post-v1 or wave 7; `transport=direct` rare until then |
| **B — Peer-direct required for v1** | Wave 4d blocks release; libp2p integration work is on critical path |

**Your choice:** ☐ A ☐ B — notes: _______________

---

## 4. Platform safety limits timing

**Question:** When to land [platform-safety-limits](platform-safety-limits/) (LLM HTTP caps, profile JSON bounds)?

Open ADRs in [platform-safety-limits/DECISIONS.md](platform-safety-limits/DECISIONS.md):

| ID | Question | Options |
|----|----------|---------|
| O001 | `PlatformLimits.h` location | `src/common/` vs `src/base/platform/` |
| O002 | Oversize LLM response | Hard error vs truncate + user-visible notice |
| O003 | Schedule vs chat v2a | Parallel now vs after SqliteThreadStore *(store is done — parallel with v6/c2 is reasonable)* |
| O004 | Max contacts count | Optional cap (e.g. 10_000) or unbounded |

**Your choices:**

- O001: ☐ `src/common/` ☐ `src/base/platform/`
- O002: ☐ hard error ☐ truncate + notice
- O003: ☐ parallel with v6/e2e ☐ defer until after v1 batch
- O004: ☐ cap at ______ ☐ no cap

**When resolved:** Move rows to numbered decisions in platform-safety-limits/DECISIONS.md.

---

## 5. Group wire shape (O008)

**Question:** Only needed if expanding to **group E2E** before post-v1 freeze.

| ID | Question | Options (from [DECISIONS](chat-storage-and-memory/DECISIONS.md#open-decisions-not-yet-resolved)) |
|----|----------|--------|
| O008 | Group pairwise wire shape | N ciphertexts per message; sender-keys tree; encrypted fan-out via relay |

**Policy already decided (E022):** pairwise sender-keys, not shared group PSK, not MLS in first slice. **O008** is the wire encoding only.

**Your choice:** ☐ defer (v1 private E2E) ☐ resolve now — notes: _______________

---

## Already decided — confirm only if you want to change

These are recorded ADRs; agents should not re-litigate. Change only with explicit product decision + new DECISIONS entry.

| Topic | Decision | ADR |
|-------|----------|-----|
| Private PSK UX | Generate/import + **mandatory fingerprint confirm** before send | E011 |
| Multi-device | **Unsupported v1** — seq conflicts → integrity pause | D015 |
| PSK at rest | **Not encrypted** in v1 | E008 |
| Signing key trust | Relay directory + cache; fingerprint **display-only** v1 | E016 |
| Strict vs relaxed ingest | **`e2e` strict**; **`e2e_public`/group relaxed when shipped | D013, D046 |
| Scroll backfill | **Deferred** post-v1 | D052 |
| Protocol/crypto for v1 private path | **Closed** (O001–O007 resolved in e2e) | e2e DECISIONS |

**Change any of the above?** ☐ No ☐ Yes — which: _______________

---

## Deferred — no decision needed for v1 batch

Documented as `[future]` or `[post-v1]` in DESIGN/PHASES:

- Scroll-triggered history backfill (post-v6c / D052)
- Per-message transport badges (post-v6d)
- Shared `@ai+` / `@ai++` (post-v6b)
- Rich ChatPayload beyond text/system (post-v4)
- `sender_instance_id` / multi-device protocol (D074)
- Cross-thread FTS search
- Blockchain attestation for signing keys (D091 — post-v1)
- Display-order escape hatches (D077 — accepted complexity budget for v1)
- PQ library choice (c4 — liboqs vs OpenSSL OQS vs BoringSSL PQ)

---

## Changelog

| Date | Change |
|------|--------|
| 2026-07-02 | Created from pre-rollout decision review (human checklist) |
