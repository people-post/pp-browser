# Phased roadmap

**[DESIGN.md](DESIGN.md) is the authoritative specification** (complete system with `[v1]` / `[post-v1]` tags). **This file orders work only** — checklists, exit criteria, and traceability. Rationale: [DECISIONS.md](DECISIONS.md).

This file has **two orderings**:

| Ordering | Use when |
|----------|----------|
| **Phase sections below** (v2a → v2b → …) | Incremental rollout, traceability, exit criteria |
| **[Agent batch delivery](#agent-batch-delivery-order)** | Agents finish all `[v1]` work before a single release — parallel waves, merged gates |

Before each phase, read [DESIGN.md § Implementer constraints](DESIGN.md#implementer-constraints) and [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md).

Check boxes when work is **merged and verified**. Add sub-items freely; keep phase boundaries stable unless DECISIONS records a change.

---

## Traceability

| Phase | Agent wave | Primary DESIGN sections | Maturity shipped |
|-------|------------|-------------------------|------------------|
| v2a | 1–2 | [Data model](DESIGN.md#data-model-target), [On-disk layout](DESIGN.md#on-disk-layout-target--d025-d028-d035-d036), [Store interface](DESIGN.md#store-interface-target), [Clear / forget](DESIGN.md#clear--forget-semantics-user-facing--d024), [Resource bounds](DESIGN.md#resource--trust-bounds-d029d033) | `[v1]` SQLite + transcript |
| v2b | 2 | [Thread / tier](DESIGN.md#three-chat-tiers-d089), [UI sidebar](DESIGN.md#sidebar-v1) | `[v1]` tier **data model** (shells + badges); functional `e2e_public` gated until c3 |
| v3 | 3 | [Durable memory](DESIGN.md#durable-memory-per-thread), [Three layers](DESIGN.md#three-layers-transcript-vs-context-vs-memory) | `[v1]` compaction + forget |
| v4 | 3 | ChatPayload **validation** + transport column (wire unchanged since v2a-p2p, D063) | `[v1]` text/system + transport column |
| v6 | 4 | [P2P sync](DESIGN.md#p2p-sync-e2e-only--d045), [FetchChatTargetMessages](DESIGN.md#unified-backfill--fetchchattargetmessages-d058), [Peer-direct fetch](DESIGN.md#peer-direct-history-fetch-d060), [Integrity recovery](DESIGN.md#integrity-recovery-d038), [Durable outbox](DESIGN.md#durable-outbox-d017) | `[v1]` E2E tail + gap + user sync |
| post-v4 | 6 | [ChatPayload](DESIGN.md#chatpayload-unified-message-body--d026) (`[post-v1]` rows) | Rich payload types |
| post-v6b | 6 | [`@ai` modes](DESIGN.md#ai-in-direct-threads-d012) | Shared `@ai+` / `@ai++` |
| post-v6c | 6 | [P2P sync — scroll backfill](DESIGN.md#p2p-sync-e2e-only--d045) | Scroll trigger on D058 |
| post-v6d | 6 | [Transport provenance](DESIGN.md#transport-provenance-d051) | Per-message badge UI |
| post-v6e | 7 | *(merged into public tier milestone)* | Relaxed ingest ships with `e2e_public` / group per D046 — see e2e **c3+** |

**Cross-project:** [e2e-message-crypto](../e2e-message-crypto/PHASES.md) — c1 in wave 1 (parallel); c2–c3 in waves 5–6 after v6.

---

## Agent batch delivery order

For **batch delivery** (agents complete work before one release), use this section. Phase checklists below are unchanged — waves tell you **what to run in parallel** and **which rollout gates to skip**.

### Scope buckets

| Bucket | Phases | Notes |
|--------|--------|-------|
| **v1 private E2E release** | v2a–v6 + [e2e c1–c3](../e2e-message-crypto/PHASES.md) | Minimal private-only batch |
| **v1 + post-v1 polish** *(chosen — [D092](DECISIONS.md#d092--release-scope-bucket-b))* | + post-v4, post-v6b/c/d | **Current release target**; peer-direct D060 required ([D094](DECISIONS.md#d094--peer-direct-history-required-for-v1-d060)) |
| **Full three-tier product** | + `e2e_public` auto-key (e2e c3+), group (E022/D095) | Not in v1 checklists |
| **PQ** | e2e c4 | Deferred — exclude unless scope expands |

### Work waves

```
Wave 1 (parallel)
  Agent A: v2a-core
  Agent B: e2e c1                    ← see e2e PHASES § Agent batch

Wave 2
  v2a-p2p + v2b data model (merge)   ← one branch; skip e2e_public “coming soon” gate

Wave 3 (parallel)
  Agent A: v3
  Agent B: v4

Wave 4 — split v6 (sequential sub-packages; tests with each)
  v6-schema → v6-pipeline → v6-sync → v6-libp2p → v6-integrity

Wave 5–6 (e2e, after v6)
  c2 → c3

Wave 7 (optional post-v1)
  post-v6c, post-v6d, post-v4, post-v6b, e2e_public functional + post-v6e
```

| Wave | Chat-storage work | Parallel / blocked by |
|------|-------------------|------------------------|
| **1** | **v2a-core** — SQLite, full schema (nullable future cols), AI transcript, `GetMessagesPage`, `GetMessagesForContext`, clear-history UX | **Parallel:** [e2e c1](../e2e-message-crypto/PHASES.md#phase-c1--basecrypto-groundwork) |
| **2** | **v2a-p2p + v2b** — final `RelayEnvelope`, `chat_targets`, outbox, `ChatTargetKey` routing, `ThreadChannel`, minimal `ChatPayloadCodec`, tier badges | After v2a-core checkpoint |
| **3** | **v3** *or* **v4** (independent) | After wave 2; run both in parallel |
| **4a** | **v6-schema** — `sender_seq`, `session_epoch`, `sync_state`, `chat_targets` PSK columns, `GetMessagesBySeqRange` | v2b + v4 |
| **4b** | **v6-pipeline** — outbox, receive classifier, `ReplayWindow`, inbound find-only (D062), floor semantics | 4a |
| **4c** | **v6-sync** — `FetchChatTargetMessages`, tail/gap, empty-gap guard (D067), user sync (D059) | 4b |
| **4d** | **v6-libp2p** — `/pp-browser/chat-history/1.0.0` (**release-critical** — [D094](DECISIONS.md#d094--peer-direct-history-required-for-v1-d060)) | 4c |
| **4e** | **v6-integrity** — compromised freeze (D068), epoch bump txn, passive adopt (D085), banners | 4b + c1 `ReplayWindow` |
| **5–6** | *(e2e)* c2 → c3 | After v6 exit; see [e2e PHASES](../e2e-message-crypto/PHASES.md#agent-batch-delivery-order) |
| **7** | post-v6c, post-v6d, post-v4, post-v6b; `e2e_public` + post-v6e with e2e c3+ | After v1 + c3 |

**Wave checkpoints** (grep/tests — same as phase exit criteria):

- **Wave 1:** AI thread survives restart; no `GetMessages` in `src/feature/` (D057); e2e c1 vector tests green — **done**
- **Wave 2:** New relay envelope on send/receive (D063); per-thread `HasMessageId`; outbox reconciliation — **done** (plaintext payload until c2; interim JSON signing)
- **Wave 3:** Summary in LLM context (v3 core); remote `content_rml` stripped (v4)
- **Wave 4:** Private `e2e` gap repair + user sync; integrity UX; cross-cutting tests in [§ Cross-cutting tasks](#cross-cutting-tasks)
- **Wave 5–6:** Two devices exchange ciphertext via relay; PSK verify + rotation (c3)

### Rollout gates to skip in batch mode

| Gate (incremental rollout) | Batch-mode alternative |
|----------------------------|-------------------------|
| v2a-core **must merge before** v2a-p2p | Keep as **validation checkpoint**, not a release gate — one agent may do both in one branch |
| `e2e_public` compose disabled until c3 (v2b) | Omit public contact action until c3+, or wire with test PSK — do not build “coming soon” UX |
| Per-phase doc updates | Batch doc updates per **wave** |
| v3 before v4 ordering | **Parallel** after wave 2 |

### Agent session reading list

Give each agent **only** the slice it needs:

1. [DESIGN.md § Implementer constraints](DESIGN.md#implementer-constraints) — always
2. [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) — wire, codec, history fetch
3. [docs/MESSAGE_ENCRYPTION.md](../../docs/MESSAGE_ENCRYPTION.md) — when touching crypto or coordinating with e2e
4. Relevant **phase checklist** section(s) in this file
5. [CURRENT_STATE.md](CURRENT_STATE.md) — update in the same PR

### Anti-patterns (cause rework)

- `GetMessages` in `src/feature/` (D057)
- Legacy `envelope.thread_id` coexisting with v2a-p2p shape (D063)
- v6 sync before `GetMessagesBySeqRange` exists
- Partial `thread.db` schema — create **all** columns at v2a (DESIGN implementer constraints)
- Crypto wire-up (c2) before `sender_seq` / `session_epoch` on envelope (AAD + E014 sign bytes)

---

## Baseline (pre-project) — done

Existing foundation this project builds on.

- [x] `IThreadStore` + `JsonThreadStore` (index + one file per thread) — **to be replaced by SqliteThreadStore in v2a**
- [x] `Thread` / `ThreadMessage` types and JSON serde
- [x] `P2pMessagingService` send, poll, message-id dedup
- [x] `MessageRouter` (AI thread, direct relay, `@ai` assist)
- [x] `AgentSession::SubmitToThread` + thread context policy
- [x] Sliding window / turn coordinator for AI
- [x] Sidebar: list threads, new AI thread, close (= delete) thread
- [x] Docs: [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md)

---

## Phase v2a — SQLite storage + unified transcript

**Goal:** Replace `JsonThreadStore` with **`SqliteThreadStore`** (D028); one durable path for all AI chat; clear-history API; no silent loss on restart.

**Design refs:** [On-disk layout](DESIGN.md#on-disk-layout-target--d025-d028-d035-d036), [Startup reconciliation](DESIGN.md#startup-reconciliation-d047), [Implementer constraints](DESIGN.md#implementer-constraints), [SQLite operations](DESIGN.md#sqlite-operations-d044), [Clear confirmation](DESIGN.md#clear-messages--confirmation-dialog-d057).

**Sub-phases (D057):** Land **v2a-core** first (vertical slice: AI threads), then **v2a-p2p** (direct messaging plumbing). Do not merge v2a-p2p until v2a-core exit criteria pass. **Agent batch:** wave 1 = v2a-core; wave 2 merges v2a-p2p with [v2b](#phase-v2b--private-vs-public-e2e-tier-split-d089) — rollout merge gate optional.

### v2a-core — SQLite + AI transcript

**Goal:** Prove persistence and UI windowing on AI threads before P2P cutover.

#### Vendor SQLite (`pp_base`)

**Note:** `third_party/sqlite` and `cmake/dependencies.cmake` already vendored — v2a-core completes **link to `pp_base`** and `SqliteThreadStore`.

- [x] SQLite in `third_party/` (amalgamation) — **not** libp2p fork SQLite
- [x] Link `pp_base` to SQLite in [src/base/CMakeLists.txt](../../src/base/CMakeLists.txt)
- [ ] Document in [BUILD.md](../../docs/BUILD.md) if not already noted

#### `ThreadMessage` + store types (D066)

- [x] Add **`display_order`** (`int64_t`) to `ThreadMessage` in `ThreadTypes.h`; serde + `AppendMessage` / `GetMessagesPage`
- [x] SQLite schema includes all future columns (nullable unused); C++ wires **`display_order`** first

#### `SqliteThreadStore` — AI path (D028)

- [x] Layout: `threads/profile.db` (`threads` + `outbox` + `chat_targets` schema), `threads/{id}/thread.db`, `{id}/blobs/` placeholder dir optional (D075) — no `index.json` (D035)
- [x] `thread.db`: `messages` (+ **`chat_payload`** BLOB D078/D087, **`display_order`**, **`chat_actions`**), `memory`, `sync_state` tables; **`idx_messages_display`** (D054)
- [x] **`ChatPayloadCodec::EncodeToRow`** — canonical `chat_payload` BLOB + denormalized columns atomically (D078/D087)
- [x] Legacy JSON wipe on first v2a run (D016); `user_version=1` schema (D069)
- [x] Lazy-open `thread.db`; WAL; writer mutex per DB
- [x] `AppendMessage` / `UpdateMessage`: assign **`display_order`** (D054); maintain `threads.updated_at` / `unread_count` in `profile.db`
- [x] `ListThreads`: catalog from `profile.db`
- [x] Profile open: orphan `thread.db` repair stub (D035)
- [x] `GetMessagesPage(thread_id, before_display_order, limit)` (D031, D054)
- [x] `GetMessagesForContext` — tail slice; wire `AgentSession` (D039); no full-thread `GetMessages` in feature code (D057 grep gate)
- [x] `ClearMessages` — purge outbox; reset preview/unread (D024); floor stub for non-E2E (D037)
- [x] `DeleteThread` — AI: full remove
- [x] Wipe legacy JSON on first run (D016)
- [x] Wire bootstrap to `SqliteThreadStore`
- [x] `MessagingLimits` on append (D029)
- [ ] LRU open `thread.db` handles (D029) — basic LRU present; tune if needed

#### Agent / chat integration (core)

- [ ] Remove or gate legacy `agent_->Submit()` — always `SubmitToThread` for AI threads
- [ ] `StartNewConversation()` deprecated or mapped to new thread creation only
- [x] On open AI thread: `GetMessagesPage` + `BuildDisplayRows` (D031)
- [x] Persist assistant `content_rml` + `chat_actions`
- [x] Extend `IThreadStore` API in one pass; migrate all `src/feature/` call sites (D057)

#### UX (core)

- [ ] Thread menu: **Clear history** → choice sheet → **confirmation dialog** with pre-clear inventory (D024, D057)
- [ ] Forget-AI checkbox on choice sheet; **AI memory retained** section when unchecked (D064)
- [ ] Composer maxlength (`kMaxComposeTextBytes`, D029)

**v2a-core exit criteria:** AI thread survives restart; clear-messages shows confirmation then empties UI; sidebar preview/unread reset; `GetMessagesPage` works; no `GetMessages` in feature code.

---

### v2a-p2p — Direct messaging storage

**Goal:** `chat_targets`, outbox, per-thread dedup, `FindOrCreateDirectThread`. **Final E2E relay wire shape** (D063/D090) — no second cutover at v4.

#### Wire + C++ types (D063, D066, D072)

- [x] **`RelayEnvelope`:** remove `thread_id`; add **`envelope_version`**, `sender_contact_id`, `route`; **`body.e2e.payload_b64`** (D090)
- [x] **`ChatHistoryRequest` / `ChatHistoryResponse`** — shared structs per [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) (D072)
- [x] Send/post and poll ingest use new envelope; **reject** legacy `thread_id`, flat `body.text`, unknown **`envelope_version`**
- [x] **Grep gate:** no `envelope.thread_id` in `src/feature/` or `tests/` (legacy rejection tests excepted)
- [x] Minimal ChatPayload codec on wire: **`RelayWirePayload`** (plaintext bytes in `payload_b64` until c2) (D073)

#### Storage + routing

- [x] `profile.db`: **`chat_targets`**, **`outbox`** populated; **`threads.group_id`** nullable column (D076)
- [x] **`FindOrCreateDirectThread(DirectChatTarget)`** — **outbound only**; inbound lookup existing target (D062)
- [x] **Startup reconciliation** — outbox ↔ messages (D047)
- [x] `HasMessageId(thread_id, message_id)`; update `P2pMessagingService` poll path (D034)
- [x] `DeleteThread` — direct: keep **`chat_targets`** (D056)
- [x] Unit tests: DirectChatTarget routing, reject wire `thread_id`, ChatPayload roundtrip, outbox reconciliation (`p2p_relay_wire_test.cpp`)
- [x] **Close conversation** = delete thread + `profile.db` cleanup (outbox rows; `chat_targets` link cleared)

### Docs

- [ ] Update [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) persistence section
- [x] Update [CONFIGURATION.md](../../docs/CONFIGURATION.md) on-disk layout
- [x] Update this file + README progress snapshot

**Exit criteria:** v2a-core criteria + per-thread `HasMessageId`; outbox reconciliation passes; clear cancels pending outbox rows; **new relay envelope** on send/receive (D063).

---

## Phase v2b — Private vs public E2E tier split (D089)

**Goal:** Same contact can have two isolated **E2E** direct thread **shells** — private (strict, functional in v6) and public (UX-first target, **gated** until auto-key c3+).

**Design refs:** [Three chat tiers](DESIGN.md#three-chat-tiers-d089), [Thread](DESIGN.md#thread), [Sidebar `[v1]`](DESIGN.md#sidebar-v1), [Chat target](DESIGN.md#chat-target-long-lived-direct-p2p).

### Model

- [x] Add `ThreadChannel` enum (`e2e`, `e2e_public`)
- [x] `channel` on `Thread` + `profile.db` `threads`; `encrypted = (channel ∈ { e2e, e2e_public })`
- [x] `FindOrCreateDirectThread(DirectChatTarget)` — replace single-key lookup (D056)
- [x] Wipe any remaining legacy JSON thread files (D016)

### Creation flows

- [x] Contact action **Secure message** → `e2e` (private direct — functional when c2+v6 land)
- [x] Contact action **Message** → `e2e_public` shell only — **disable compose/send** until e2e **c3** auto-key (show “coming soon” or equivalent)

### UI

- [x] Sidebar **flat list** with **Public / Private** tier badge per direct row (D023)
- [x] E2E shell styling for both tiers (`.chat-shell--e2e` / tier variant via `thread_encrypted`)

### Memory boundary

- [ ] AI context and memory never cross tiers (per `thread_id` / `thread.db`) — structural separation only; add explicit tests/guards in v3

**Exit criteria:** Two thread dirs + DBs for one contact (private + public tier shells); sidebar shows tier badge on each row; **`e2e_public` send/receive not enabled** until cross-project c3.

---

## Phase v3 — Durable AI memory and forget semantics

**Goal:** Long-running AI threads compact gracefully; user can forget without losing transcript.

**Design refs:** [Durable memory](DESIGN.md#durable-memory-per-thread), [Clear / forget](DESIGN.md#clear--forget-semantics-user-facing--d024), [Resource bounds — compaction](DESIGN.md#resource--trust-bounds-d029d033).

### Memory storage

- [x] **`ConversationSummary`** JSON schema + `memory` key namespace (D070)
- [x] `IThreadStore::GetThreadMemory` / `SetThreadMemory` / `ClearThreadMemory`
- [x] Wire `ThreadContextPolicy` to inject summary when present (D039)

### Compaction (minimal v3 — D040)

- [x] `ThreadCompactionService` — when text turn count since last summary > **`kCompactionTurnThreshold` (20)**
- [x] Async job after turn completes; `kMaxSummaryBytes` (8 KiB) on persist
- [x] `GetMessagesForContext` respects compaction cursor + tail (`kCompactionMinTurnsKept` = 6)
- [x] **`ThreadContextPolicy`:** filter to `content_type=text` (+ selected `system`) via store query (D039, D057)

### UX

- [ ] **Forget what AI learned** — `DELETE FROM memory` (or summary key), keeps `messages` — store API done; menu deferred
- [ ] Clear history: optional **Also forget what AI learned** checkbox (D024)
- [ ] P2P disclosure copy on clear levels

### Docs

- [ ] Extend [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) compaction section

**Exit criteria:** 20+ turn AI thread uses summary in LLM context; forget memory clears `memory` table but leaves messages.

---

## Phase v4 — ChatPayload validation hardening + transport column

**Goal:** Full **ChatPayload validator** (D026, D050) on the **same wire shape** shipped in v2a-p2p (D063) — no envelope break. Strip remote `content_rml`; persist `transport` (badge UI in post-v6d).

**Design refs:** [ChatPayload `[v1]`](DESIGN.md#chatpayload-unified-message-body--d026), [Wire cutover phasing](DESIGN.md#wire-cutover-phasing-d063), [Transport `[v1]`](DESIGN.md#transport-provenance-d051), [Three chat tiers](DESIGN.md#three-chat-tiers-d089).

### Message schema

- [ ] `content_type` + `payload` on **`ThreadMessage`** C++ + store read/write; **`system`** type support
- [ ] ChatPayload binary codec **validator** hardening (D029, D050) — unknown types rejected on ingest
- [ ] Reject oversize payload on send; strip wire `content_rml` on ingest (D030)
- [ ] Direct wire: **`body.e2e.payload_b64` only**; reject `public_relay` / `content_b64` (D090)
- [ ] `transport` column; set in send/receive paths (no per-message badge UI yet, D051)

### LLM / display

- [ ] `ThreadContextPolicy` filters to `content_type=text` (+ selected `system`)
- [ ] Text bubbles only in v4

### Protocol

- [ ] Dedup by `message_id` per thread (`thread.db` PK, D034)
- [ ] Ingest: **participant check** on `sender_contact_id` (D027)

**Exit criteria:** Structured text payload survives restart; remote `content_rml` stripped; ingest branches on tier (D089).

---

## Phase v6 — Private direct seq, tail sync, gap repair, and user sync

**Agent batch:** Split into sub-packages **v6-schema → v6-pipeline → v6-sync → v6-libp2p → v6-integrity** — see [§ Agent batch delivery](#agent-batch-delivery-order) wave 4. Land tests with each sub-package.

**Goal:** **Private direct (`e2e`)** chat detects missing peer messages and syncs reliably. **`e2e_public`** uses same sync machinery when that tier ships (relaxed ingest — D046). Send failure keeps local copy (D017); peer sync fills **receive-side** gaps (D058–D059).

**Depends on:** v2b, v4; relay `GET /v1/chat-targets/messages` (D027, D056) with **chat-target authorization**; libp2p history protocol (D060) when direct transport available.

**Design refs:** [P2P sync `[v1]` modes](DESIGN.md#p2p-sync-e2e-only--d045), [FetchChatTargetMessages](DESIGN.md#unified-backfill--fetchchattargetmessages-d058), [User-initiated sync](DESIGN.md#user-initiated-sync-ux-d059), [Peer-direct fetch](DESIGN.md#peer-direct-history-fetch-d060), [Integrity `[v1]`](DESIGN.md#v1-recovery), [Epoch bump](DESIGN.md#epoch-bump-transaction-d014-cross-project), [Durable outbox](DESIGN.md#durable-outbox-d017).

### v6-schema — Schema and persistence

- [x] `sender_seq`, `session_epoch` on **E2E** messages + envelope (D021, D045)
- [x] `sync_state` table populated per `(peer, session_epoch)` — E2E threads only
- [x] **`chat_targets` in `profile.db`** (D047) — seq/epoch + PSK columns (D084); not JSON sidecar
- [x] `GetMessagesBySeqRange` on `IThreadStore` for tail/gap
- [x] Assign `(message_id, sender_seq)` at first local persist on E2E send; serialize per chat target

### v6-pipeline — Send / receive

- [x] Sign envelope via `EnvelopeSigner` (E014) including E2E seq fields
- [x] Durable outbox: `ListPendingOutbox()` + reconciliation on startup (D017, D047)
- [x] **Private (`e2e`):** receive pipeline D013/D033; inbound **find-only** (D062/D080); **`ReplayWindow` helper**, classifier authoritative (D020)
- [ ] **`e2e_public`:** out of scope for v6 — same pipeline when public tier ships (D089); includes auto-create (D080) + relaxed ingest (D046)
- [x] Inbound Ed25519 verify + **`PeerSigningKeyStore`** lookup (E016, D081); strip remote `content_rml` (D030)
- [x] Poll backoff min 2 s foreground (D032); cap poll batch (D029)
- [ ] Outbox retry **`kMaxOutboxRetryAttempts`**; gap repair **`kMaxGapRepairRounds`** / **`kMaxGapRepairSeqSpan`** (D041)
- [x] Clear history → `history_floor_seq = loaded_max_seq` (not contiguous-only); below-floor silent discard (D037)
- [ ] Peer reset / integrity recovery — **no continue-anyway** (D014, D038, D046)
- [x] **Epoch bump transaction** with e2e crypto sessions (D047); **passive epoch adopt** on first ingest when peer bumps first (D085); **rich OOB bundle** export/import (D086/E020)

### v6-sync — Sync modes (E2E only — D052, D058, D059)

- [x] **`FetchChatTargetMessages`** — unified backfill; peer-direct (D060) then relay D027 (D058)
- [x] **Tail sync** — desc limit 50
- [x] **Gap repair** — automatic via D058
- [x] **Authoritative empty gap close** — success + zero messages closes hole **only when D067 guard passes**; `empty_closed_seqs[]` / `empty_closed_ranges[]` + late fill (D061/D067/D071)
- [x] **Compromised thread (D068)** — outbox frozen; no gap/tail sync; epoch bump cancels old-epoch pending; coordinator updates `chat_targets` PSK + epoch in one `profile.db` txn (D084)
- [x] **User-initiated sync** — thread menu **Sync with peer**; gap banner **Retry sync** (D059)
- [ ] Gap repair assigns **`display_order`** between seq neighbors (D054 Rule 2) — deferred D065 partial
- [ ] **Gap repair UI defer** — D065: skip refresh above window; defer + anchor when renumber touches loaded page
- [x] Reorder buffer / `ReplayWindow` k=32 (D020)
- [x] Persist **`loaded_min_seq` / `loaded_max_seq`** watermarks in `sync_state` (prerequisite for post-v6c)

### v6-libp2p — libp2p peer-direct (D060)

- [x] Protocol **`/pp-browser/chat-history/1.0.0`** — request/response mirrors D027
- [x] Responder serves `GetMessagesBySeqRange` from local `thread.db`
- [x] Requester ingests envelopes; `transport=direct` on persist

### v6-integrity — Integrity and UX

- [x] E2E gap / compromised banners; choice sheet: rotate PSK or pause only (D046) — epoch-only bump until c3 PSK rotation
- [x] **Sync with peer** + **Retry sync** copy: peer sync ≠ retry unsent (D059)
- [x] Unit tests: seq, outbox, floor (`loaded_max_seq`), epoch, reorder, reconciliation, clear-after-gap-repair no resurrection, **empty gap close guard + late fill (D067)**, **inbound find-only (D062)**, **compromised outbox freeze (D068)**, **pending cancel on epoch bump (D068)**, **passive epoch adopt txn (D085)**, **PSK bundle merge + ledger cap (D086/E020)** — partial: `v6_integrity_test` covers bump/adopt/compromised; D086 bundle tests deferred to e2e c3

### Docs

- [ ] Extend [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md): envelope limits (D029), verify pipeline, relay auth (D027), [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md)
- [ ] Document single-active-device (D015, E2E only)

**Exit criteria:** **Private (`e2e`)** gap auto-repair; **user-initiated sync** via relay (direct when libp2p up); outbox survives restart; strict integrity UX only (no relaxed ingest); `e2e_public` shells unaffected (not message-functional until c3).

---

## Phase post-v6c — Scroll-triggered history backfill (D052)

**Goal:** Add **scroll-to-top** as a trigger for the existing **`FetchChatTargetMessages`** primitive (D058) — [DESIGN § P2P sync](DESIGN.md#p2p-sync-e2e-only--d045).

**Depends on:** v6 watermarks, D058/D060/D027.

- [x] Scroll to top invokes D058 older-range fetch — page `(history_floor_seq, loaded_min_seq)`
- [x] Same ingest + floor rules as user-initiated sync (D059)
- [x] Scroll hint UX when older history may exist

---

## Phase post-v4 — Rich ChatPayload types

**Goal:** Enable `[post-v1]` payload types from [DESIGN § ChatPayload](DESIGN.md#chatpayload-unified-message-body--d026).

**Depends on:** v4 validator + `BuildDisplayRows` branching.

- [x] Validator accepts `annotation`, `contact_card`, `crypto_tx` on inbound relay
- [x] Type-specific UI templates; `BuildDisplayRows` merge for annotations
- [x] Orphan target badge (D043); annotation cap **`kMaxAnnotationsPerTarget`** (D042)
- [x] LLM context unchanged unless summarized in `text`

---

## Phase post-v6b — Shared `@ai` modes

**Goal:** `@ai+` / `@ai++` per [DESIGN § `@ai`](DESIGN.md#ai-in-direct-threads-d012).

**Depends on:** v6 E2E send pipeline + seq on `relay_visible` rows.

- [x] Parser for `@ai+`, `@ai++` (and optional long-form aliases)
- [x] `generation`, `seq_owner_contact_id`; trigger user owns `sender_seq`
- [x] Shared reply (+1 seq) and shared full (+2 seq) flows
- [x] Confirm UX before first shared send

---

## Phase post-v6d — Per-message transport badge UI (D051)

**Goal:** [DESIGN § Transport — `[post-v1]`](DESIGN.md#transport-provenance-d051).

**Depends on:** v4 `transport` column populated; libp2p direct path when available.

- [x] E2E per-message Direct / Relay / Local indicator
- [x] Read `transport` column; no inference from thread type alone

---

## Phase post-v6e — Relaxed ingest (merged — see public tier)

**Status:** **Merged into `e2e_public` / group tier milestone** (D046). Relaxed ingest is the **default** policy when those tiers ship — not a separate optional phase after private v6.

**When enabling public direct (e2e c3+):**

- [ ] `ingest_policy=relaxed`, `user_resolution=continue_anyway`, `trust_degraded` in `sync_state.state_json`
- [ ] Choice sheet secondary action after disclosure
- [ ] Hard crypto failures still have no override

**Design ref:** [DESIGN § Relaxed ingest](DESIGN.md#relaxed-ingest--continue-anyway--public-direct-and-group-d046)

---

## Cross-project — E2E crypto wire-up

**Not a chat-storage phase** — tracked in [e2e-message-crypto](../e2e-message-crypto/PHASES.md).

| Chat-storage gate | E2E phase | Work |
|-------------------|-----------|------|
| v2b channel split | c2 | `P2pMessagingService` encrypt/decrypt on `channel=e2e` |
| v6 envelope + seq | c2–c3 | AAD binds `sender_seq`; `ChatPayload` plaintext (E010) |
| v6 receive pipeline | c2 | `PeerSigningKeyStore` + inbound verify (E016, D081) |
| v6 peer history (D060) | libp2p integration | `/pp-browser/chat-history/1.0.0` responder + requester |
| D038 integrity UX | c3 | PSK rotation + epoch bump transaction (D047) |

Ship SQLite storage + private-tier envelope plumbing without c2; E2E body crypto for **`e2e`** lands after v2b + v6 foundations; **`e2e_public`** auto-key + relaxed ingest at e2e **c3+**.

---

## Deferred — cross-thread FTS search `[future]`

**Goal:** Agent/search across all threads without loading every transcript.

- [ ] Optional FTS5 virtual table in a thread or profile search DB (not `profile.db`)
- [ ] Internal API only until UI needs it

**Trigger:** search feature request or agent tool needs full-text recall.

---

## Cross-cutting tasks

- [x] `SqliteThreadStore` unit tests (`profile.db`, per-thread db, clear, delete, outbox, **chat_targets**, **reconciliation**, size reject) — partial via `sqlite_thread_store_test`, `p2p_relay_wire_test`, `messaging_cross_cutting_test`
- [x] Ingest tests: oversize envelope, remote content_rml stripped (D029–D030) — `messaging_cross_cutting_test`, `chat_payload_validator_test`
- [x] Public vs E2E ingest path tests (D045) — `messaging_cross_cutting_test`, `e2e_relay_crypto_test`
- [ ] Agent tool docs if `list_conversations` must expose channel
- [x] Fuzz/dedup: duplicate relay `message_id` ignored via per-thread store check — `messaging_cross_cutting_test`
- [x] `GetMessagesForContext` / compaction tests (D039–D040) — `sqlite_thread_store_test`
- [x] Outbox/gap repair limit tests (D041) — `chat_sync_test` gap clamp; outbox in `p2p_relay_wire_test`
- [x] Relay fetch 403 for non-party chat target (D027) — `chat_sync_test` mock 403; live via `relay_live_integration_test` env
- [x] **`FetchChatTargetMessages`**: peer-direct then relay; empty gap close with D067 guard + late fill (D058–D061/D067) — `chat_sync_test`
- [x] User-initiated sync + inbound find-only + compromised freeze tests (D059, D062, D068) — `chat_sync_test`, `messaging_cross_cutting_test`, `v6_integrity_test`

---

## Changelog

| Date | Change |
|------|--------|
| 2026-06-27 | Project doc created from planning discussion |
| 2026-06-27 | D008–D011: sender seq, windowed sync, seq lifecycle, session compromise; phase v6 |
| 2026-06-27 | D012: three `@ai` modes; phase v6b; sync seq only when `relay_visible` |
| 2026-06-29 | D013–D014: strict normal-or-compromised ingest, peer reset = epoch bump; DESIGN P2P sync rewrite |
| 2026-06-29 | D015–D022: single active device, no migration, durable outbox, public ingest, display order, reorder window, sender_contact_id, receive pipeline |
| 2026-06-29 | D023–D027: sidebar groups, clear choice sheet, ChatPayload, relay API |
| 2026-06-29 | D034: per-thread message_id dedup; profile.db outbox-only |
| 2026-06-29 | D028: SQLite per thread + profile.db from v2a; drop JSON message stage; v5 absorbed |
| 2026-06-29 | D035: drop `index.json`; sidebar catalog in `profile.db` `threads`; lazy visible-row verify |
| 2026-06-29 | D036: rename `registry.db` → `profile.db` |
| 2026-06-29 | D038: user-choice integrity recovery; pause + choice sheet; relaxed ingest; hard vs soft failures |
| 2026-06-29 | D037: clear history floor = sync exclusion + silent discard; amend D010/D013 |
| 2026-06-29 | D039–D044: agent context tail read, compaction bounds, retry/repair caps, annotation cap, orphan UX, SQLite ops |
| 2026-06-29 | D056: local `thread_id`; wire `ChatTargetKey` + `route`; no `thread_id` on envelope/AAD; supersedes D053 |
| 2026-06-29 | DESIGN: single grand spec with `[v1]`/`[post-v1]` tags; PHASES traceability + named post-v1 phases |
| 2026-06-30 | D063–D066: wire cutover v2a-p2p, clear AI memory retained copy, display_order UI defer, C++ type gates; D057/D054 amended |
| 2026-06-30 | D058–D062: unified `FetchChatTargetMessages`, user-initiated sync, libp2p peer history, empty gap close, inbound find-only; D009/D052/D041/D022 amended |
| 2026-06-29 | D069–D078: schema evolution (migrate vs wipe), `chat_payload` canonical body, `envelope_version`, WIRE_SCHEMAS, memory JSON schema, empty-closed-seq cap, blobs/group placeholders, unknown-field policy, display_order complexity budget |
| 2026-07-02 | D089/D090 doc alignment: receive pipeline auto-create (D080), remove stale public-relay ingest; phasing split v2b shells vs functional `e2e_public`; relaxed ingest merged into public tier milestone; `payload_version` naming |
| 2026-06-30 | D067–D068: empty gap close guard + late fill; compromised outbox/sync freeze; epoch bump pending cancel; receive pipeline linearized |
| 2026-07-02 | Agent batch delivery order — parallel waves, v6 sub-packages, rollout gates to skip; traceability **Agent wave** column |
| 2026-07-02 | **Waves 1–2 landed:** v2a-core + v2a-p2p + v2b in tree; `CURRENT_STATE.md` next-agent section; interim plaintext `payload_b64` + JSON signing until c2/E014 |
| 2026-07-02 | Pre-implementation doc hygiene — CURRENT_STATE accuracy, D027 query params, `MessagingLimits.h` path, v6 sub-headings |
| 2026-07-06 | Waves 3–7 landed — v3/v4/v6, e2e c2/c3, post-v4/6b/c/d; cross-cutting tests + D093 live relay env gate |
