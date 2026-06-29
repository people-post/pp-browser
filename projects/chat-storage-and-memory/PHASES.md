# Phased roadmap

**[DESIGN.md](DESIGN.md) is the authoritative specification** (complete system with `[v1]` / `[post-v1]` tags). **This file orders work only** — checklists, exit criteria, and traceability. Rationale: [DECISIONS.md](DECISIONS.md).

Before each phase, read [DESIGN.md § Implementer constraints](DESIGN.md#implementer-constraints).

Check boxes when work is **merged and verified**. Add sub-items freely; keep phase boundaries stable unless DECISIONS records a change.

---

## Traceability

| Phase | Primary DESIGN sections | Maturity shipped |
|-------|-------------------------|------------------|
| v2a | [Data model](DESIGN.md#data-model-target), [On-disk layout](DESIGN.md#on-disk-layout-target--d025-d028-d035-d036), [Store interface](DESIGN.md#store-interface-target), [Clear / forget](DESIGN.md#clear--forget-semantics-user-facing--d024), [Resource bounds](DESIGN.md#resource--trust-bounds-d029d033) | `[v1]` SQLite + transcript |
| v2b | [Thread / channel](DESIGN.md#thread), [UI sidebar](DESIGN.md#sidebar-v1) | `[v1]` public vs E2E split |
| v3 | [Durable memory](DESIGN.md#durable-memory-per-thread), [Three layers](DESIGN.md#three-layers-transcript-vs-context-vs-memory) | `[v1]` compaction + forget |
| v4 | ChatPayload **validation** + transport column (wire unchanged since v2a-p2p, D063) | `[v1]` text/system + transport column |
| v6 | [P2P sync](DESIGN.md#p2p-sync-e2e-only--d045), [FetchChatTargetMessages](DESIGN.md#unified-backfill--fetchchattargetmessages-d058), [Peer-direct fetch](DESIGN.md#peer-direct-history-fetch-d060), [Integrity recovery](DESIGN.md#integrity-recovery-d038), [Durable outbox](DESIGN.md#durable-outbox-d017) | `[v1]` E2E tail + gap + user sync |
| post-v4 | [ChatPayload](DESIGN.md#chatpayload-unified-message-body--d026) (`[post-v1]` rows) | Rich payload types |
| post-v6b | [`@ai` modes](DESIGN.md#ai-in-direct-threads-d012) | Shared `@ai+` / `@ai++` |
| post-v6c | [P2P sync — scroll backfill](DESIGN.md#p2p-sync-e2e-only--d045) | Scroll trigger on D058 |
| post-v6d | [Transport provenance](DESIGN.md#transport-provenance-d051) | Per-message badge UI |
| post-v6e | [Relaxed ingest](DESIGN.md#post-v1-relaxed-ingest--continue-anyway-d046-extension) | Continue anyway (optional) |

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

**Sub-phases (D057):** Land **v2a-core** first (vertical slice: AI threads), then **v2a-p2p** (direct messaging plumbing). Do not merge v2a-p2p until v2a-core exit criteria pass.

### v2a-core — SQLite + AI transcript

**Goal:** Prove persistence and UI windowing on AI threads before P2P cutover.

#### Vendor SQLite (`pp_base`)

- [ ] Add SQLite to build (amalgamation or system lib) — **not** libp2p fork SQLite
- [ ] Document in [BUILD.md](../../docs/BUILD.md) / `third_party` if vendored

#### `ThreadMessage` + store types (D066)

- [ ] Add **`display_order`** (`int64_t`) to `ThreadMessage` in `ThreadTypes.h`; serde + `AppendMessage` / `GetMessagesPage`
- [ ] SQLite schema includes all future columns (nullable unused); C++ wires **`display_order`** first

#### `SqliteThreadStore` — AI path (D028)

- [ ] Layout: `threads/profile.db` (`threads` + `outbox` schema only), `threads/{id}/thread.db` — no `index.json` (D035)
- [ ] `thread.db`: `messages` (+ **`display_order`**, **`chat_actions`**), `memory`, `sync_state` tables; **`idx_messages_display`** (D054)
- [ ] Lazy-open `thread.db`; WAL + writer mutex per DB; **dual-DB recipe** — lock `profile.db` then `thread.db`, write `thread.db` txn first (D044, D057)
- [ ] `AppendMessage` / `UpdateMessage`: assign **`display_order`** (D054); maintain `threads.updated_at` / `unread_count` in `profile.db`
- [ ] `ListThreads`: catalog + visible-row verify; preview via `ORDER BY display_order DESC LIMIT 1` (D035, D054)
- [ ] Profile open: `readdir` repair — stub `threads` row for orphan `thread.db` dirs (D035)
- [ ] `GetMessagesPage(thread_id, before_display_order, limit)` (D031, D054)
- [ ] `GetMessagesForContext` — tail slice; wire `AgentSession` (D039); no full-thread `GetMessages` in feature code (D057 grep gate)
- [ ] `ClearMessages` — floor stub for non-E2E; purge outbox; `preview=''`, `unread_count=0` (D024, D037); WAL checkpoint (D044)
- [ ] `DeleteThread` — AI: full remove
- [ ] Wipe legacy JSON on first run (D016)
- [ ] Wire bootstrap to `SqliteThreadStore`
- [ ] `MessagingLimits` on append (D029); LRU open `thread.db` handles (D029)

#### Agent / chat integration (core)

- [ ] Remove or gate legacy `agent_->Submit()` — always `SubmitToThread` for AI threads
- [ ] `StartNewConversation()` deprecated or mapped to new thread creation only
- [ ] On open AI thread: `GetMessagesPage` + `BuildDisplayRows` (D031)
- [ ] Persist assistant `content_rml` + `chat_actions`
- [ ] Extend `IThreadStore` API in one pass; migrate all `src/feature/` call sites (D057)

#### UX (core)

- [ ] Thread menu: **Clear history** → choice sheet → **confirmation dialog** with pre-clear inventory (D024, D057)
- [ ] Forget-AI checkbox on choice sheet; **AI memory retained** section when unchecked (D064)
- [ ] Composer maxlength (`kMaxComposeTextBytes`, D029)

**v2a-core exit criteria:** AI thread survives restart; clear-messages shows confirmation then empties UI; sidebar preview/unread reset; `GetMessagesPage` works; no `GetMessages` in feature code.

---

### v2a-p2p — Direct messaging storage

**Goal:** `chat_targets`, outbox, per-thread dedup, `FindOrCreateDirectThread` (`public_relay` default until v2b E2E UI). **Final relay wire shape** (D063) — no second cutover at v4.

#### Wire + C++ types (D063, D066)

- [ ] **`RelayEnvelope`:** remove `thread_id`; add `sender_contact_id`, `route`; **`body.content`** minimal ChatPayload (`text` only)
- [ ] Send/post and poll ingest use new envelope; **reject** legacy `thread_id` and flat `body.text` at parse
- [ ] **Grep gate:** no `envelope.thread_id` in `src/feature/` or `tests/` (legacy rejection tests excepted)
- [ ] Minimal ChatPayload codec: serialize/parse `schema_version`, `content_type=text`, `text`, `payload={}`

#### Storage + routing

- [ ] `profile.db`: **`chat_targets`**, **`outbox`** populated (D047, D056)
- [ ] **`FindOrCreateDirectThread(ChatTargetKey)`** — **outbound only**; inbound lookup existing target (D062)
- [ ] **Startup reconciliation** — outbox ↔ messages (D047)
- [ ] `HasMessageId(thread_id, message_id)`; update `P2pMessagingService` poll path (D034)
- [ ] `DeleteThread` — direct: keep **`chat_targets`** (D056)
- [ ] Unit tests: ChatTargetKey routing, reject wire `thread_id`, minimal ChatPayload roundtrip, outbox reconciliation, clear cancels pending sends
- [ ] **Close conversation** = delete thread + `profile.db` cleanup

### Docs

- [ ] Update [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) persistence section
- [ ] Update [CONFIGURATION.md](../../docs/CONFIGURATION.md) on-disk layout
- [ ] Update this file + README progress snapshot

**Exit criteria:** v2a-core criteria + per-thread `HasMessageId`; outbox reconciliation passes; clear cancels pending outbox rows; **new relay envelope** on send/receive (D063).

---

## Phase v2b — Public vs E2E channel split

**Goal:** Same contact can have two isolated direct threads.

**Design refs:** [Thread / channel](DESIGN.md#thread), [Sidebar `[v1]`](DESIGN.md#sidebar-v1), [Chat target](DESIGN.md#chat-target-long-lived-direct-p2p).

### Model

- [ ] Add `ThreadChannel` enum (`public_relay`, `e2e`)
- [ ] `channel` on `Thread` + `profile.db` `threads`; `encrypted = (channel == e2e)`
- [ ] `FindOrCreateDirectThread(ChatTargetKey)` — replace single-key lookup (D056)
- [ ] Wipe any remaining legacy JSON thread files (D016)

### Creation flows

- [ ] Contact action “Message” specifies channel (or default public until E2E exists)
- [ ] Future: “Secure message” creates `e2e` thread when crypto ready

### UI

- [ ] Sidebar **flat list** with **Public / Private channel badge** per direct row (D023) — no collapsible groups
- [ ] E2E shell styling when `channel=e2e`

### Memory boundary

- [ ] AI context and memory never cross channels (per `thread_id` / `thread.db`)

**Exit criteria:** Two thread dirs + DBs for one contact; sidebar shows channel badge on each row.

---

## Phase v3 — Durable AI memory and forget semantics

**Goal:** Long-running AI threads compact gracefully; user can forget without losing transcript.

**Design refs:** [Durable memory](DESIGN.md#durable-memory-per-thread), [Clear / forget](DESIGN.md#clear--forget-semantics-user-facing--d024), [Resource bounds — compaction](DESIGN.md#resource--trust-bounds-d029d033).

### Memory storage

- [ ] `memory` table in `thread.db` (D028)
- [ ] `IThreadStore::GetThreadMemory` / `SetThreadMemory`
- [ ] Wire `SlidingWindowContextPolicy` to inject summary when present

### Compaction (minimal v3 — D040)

- [ ] `ICompactionService` — when text turn count since last summary > **`kCompactionTurnThreshold` (20)**
- [ ] Async job after turn completes; `kMaxSummaryBytes` (8 KiB) on persist
- [ ] `GetMessagesForContext` injects summary + tail (`kCompactionMinTurnsKept` = 6)
- [ ] **`ThreadContextPolicy`:** filter to `content_type=text` (+ selected `system`); compaction eligibility ignores non-text rows (D039, D057)

### UX

- [ ] **Forget what AI learned** — `DELETE FROM memory` (or summary key), keeps `messages`
- [ ] Clear history: optional **Also forget what AI learned** checkbox (D024)
- [ ] P2P disclosure copy on clear levels

### Docs

- [ ] Extend [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) compaction section

**Exit criteria:** 20+ turn AI thread uses summary in LLM context; forget memory clears `memory` table but leaves messages.

---

## Phase v4 — ChatPayload validation hardening + transport column

**Goal:** Full **ChatPayload validator** (D026, D050) on the **same wire shape** shipped in v2a-p2p (D063) — no envelope break. Strip remote `content_rml`; persist `transport` (badge UI in post-v6d).

**Design refs:** [ChatPayload `[v1]`](DESIGN.md#chatpayload-unified-message-body--d026), [Wire cutover phasing](DESIGN.md#wire-cutover-phasing-d063), [Transport `[v1]`](DESIGN.md#transport-provenance-d051), [Public relay ingest](DESIGN.md#public-relay-ingest-d045).

### Message schema

- [ ] `content_type` + `payload` on **`ThreadMessage`** C++ + store read/write; **`system`** type support
- [ ] ChatPayload JSON codec **validator** hardening (D029, D050) — unknown types rejected on ingest
- [ ] Reject oversize payload on send; strip wire `content_rml` on ingest (D030)
- [ ] Relay `body.content` for public; envelope signing covers structured body
- [ ] `transport` column; set in send/receive paths (no per-message badge UI yet, D051)

### LLM / display

- [ ] `ThreadContextPolicy` filters to `content_type=text` (+ selected `system`)
- [ ] Text bubbles only in v4

### Protocol

- [ ] Dedup by `message_id` per thread (`thread.db` PK, D034)
- [ ] Ingest: **participant check** on `sender_contact_id` (D027)

**Exit criteria:** Structured text payload survives restart; remote `content_rml` stripped; public ingest uses UUID dedup.

---

## Phase v6 — E2E sender seq, tail sync, gap repair, and user sync

**Goal:** **E2E direct** chat detects missing peer messages and syncs reliably. Public relay unchanged (D045). Send failure keeps local copy (D017); peer sync fills **receive-side** gaps (D058–D059).

**Depends on:** v2b, v4; relay `GET /v1/chat-targets/messages` (D027, D056) with **chat-target authorization**; libp2p history protocol (D060) when direct transport available.

**Design refs:** [P2P sync `[v1]` modes](DESIGN.md#p2p-sync-e2e-only--d045), [FetchChatTargetMessages](DESIGN.md#unified-backfill--fetchchattargetmessages-d058), [User-initiated sync](DESIGN.md#user-initiated-sync-ux-d059), [Peer-direct fetch](DESIGN.md#peer-direct-history-fetch-d060), [Integrity `[v1]`](DESIGN.md#v1-recovery), [Epoch bump](DESIGN.md#epoch-bump-transaction-d014-cross-project), [Durable outbox](DESIGN.md#durable-outbox-d017).

### Schema and persistence

- [ ] `sender_seq`, `session_epoch` on **E2E** messages + envelope (D021, D045)
- [ ] `sync_state` table populated per `(peer, session_epoch)` — E2E threads only
- [ ] **`chat_targets` in `profile.db`** (D047) — not JSON sidecar
- [ ] `GetMessagesBySeqRange` on `IThreadStore` for tail/gap
- [ ] Assign `(message_id, sender_seq)` at first local persist on E2E send; serialize per chat target

### Send / receive

- [ ] Sign envelope including E2E seq fields
- [ ] Durable outbox: `ListPendingOutbox()` + reconciliation on startup (D017, D047)
- [ ] **E2E:** receive pipeline D013/D033; inbound **find-only** (D062); **`ReplayWindow` helper**, classifier authoritative (D020)
- [ ] **Public:** UUID dedup + participant check only (D045)
- [ ] Inbound Ed25519 verify; strip remote `content_rml` (D030)
- [ ] Poll backoff min 2 s foreground (D032); cap poll batch (D029)
- [ ] Outbox retry **`kMaxOutboxRetryAttempts`**; gap repair **`kMaxGapRepairRounds`** / **`kMaxGapRepairSeqSpan`** (D041)
- [ ] Clear history → `history_floor_seq = loaded_max_seq` (not contiguous-only); below-floor silent discard (D037)
- [ ] Peer reset / integrity recovery — **no continue-anyway** (D014, D038, D046)
- [ ] **Epoch bump transaction** with e2e crypto sessions (D047)

### Sync modes (E2E only — D052, D058, D059)

- [ ] **`FetchChatTargetMessages`** — unified backfill; peer-direct (D060) then relay D027 (D058)
- [ ] **Tail sync** — desc limit 50
- [ ] **Gap repair** — automatic via D058
- [ ] **Authoritative empty gap close** — success + zero messages closes hole **only when D067 guard passes**; `empty_closed_seqs[]` + late fill on retry (D061/D067)
- [ ] **Compromised thread (D068)** — outbox frozen; no gap/tail sync; epoch bump cancels old-epoch pending; coordinator updates `sessions.json` under `profile.db` mutex
- [ ] **User-initiated sync** — thread menu **Sync with peer**; gap banner **Retry sync** (D059)
- [ ] Gap repair assigns **`display_order`** between seq neighbors (D054 Rule 2)
- [ ] **Gap repair UI defer** — D065: skip refresh above window; defer + anchor when renumber touches loaded page
- [ ] Reorder buffer / `ReplayWindow` k=32 (D020)
- [ ] Persist **`loaded_min_seq` / `loaded_max_seq`** watermarks (prerequisite for post-v6c)

### libp2p peer-direct (D060)

- [ ] Protocol **`/pp-browser/chat-history/1.0.0`** — request/response mirrors D027
- [ ] Responder serves `GetMessagesBySeqRange` from local `thread.db`
- [ ] Requester ingests envelopes; `transport=direct` on persist

### Integrity and UX

- [ ] E2E gap / compromised banners; choice sheet: rotate PSK or pause only (D046)
- [ ] **Sync with peer** + **Retry sync** copy: peer sync ≠ retry unsent (D059)
- [ ] Unit tests: seq, outbox, floor (`loaded_max_seq`), epoch, reorder, reconciliation, clear-after-gap-repair no resurrection, **empty gap close guard + late fill (D067)**, **inbound find-only (D062)**, **compromised outbox freeze (D068)**, **pending cancel on epoch bump (D068)**

### Docs

- [ ] Extend [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md): envelope limits (D029), verify pipeline, relay auth (D027)
- [ ] Document single-active-device (D015, E2E only)

**Exit criteria:** E2E gap auto-repair; **user-initiated sync** via relay (direct when libp2p up); outbox survives restart; public channel unaffected; no relaxed ingest path.

---

## Phase post-v6c — Scroll-triggered history backfill (D052)

**Goal:** Add **scroll-to-top** as a trigger for the existing **`FetchChatTargetMessages`** primitive (D058) — [DESIGN § P2P sync](DESIGN.md#p2p-sync-e2e-only--d045).

**Depends on:** v6 watermarks, D058/D060/D027.

- [ ] Scroll to top invokes D058 older-range fetch — page `(history_floor_seq, loaded_min_seq)`
- [ ] Same ingest + floor rules as user-initiated sync (D059)
- [ ] Scroll hint UX when older history may exist

---

## Phase post-v4 — Rich ChatPayload types

**Goal:** Enable `[post-v1]` payload types from [DESIGN § ChatPayload](DESIGN.md#chatpayload-unified-message-body--d026).

**Depends on:** v4 validator + `BuildDisplayRows` branching.

- [ ] Validator accepts `annotation`, `contact_card`, `crypto_tx` on inbound relay
- [ ] Type-specific UI templates; `BuildDisplayRows` merge for annotations
- [ ] Orphan target badge (D043); annotation cap **`kMaxAnnotationsPerTarget`** (D042)
- [ ] LLM context unchanged unless summarized in `text`

---

## Phase post-v6b — Shared `@ai` modes

**Goal:** `@ai+` / `@ai++` per [DESIGN § `@ai`](DESIGN.md#ai-in-direct-threads-d012).

**Depends on:** v6 E2E send pipeline + seq on `relay_visible` rows.

- [ ] Parser for `@ai+`, `@ai++` (and optional long-form aliases)
- [ ] `generation`, `seq_owner_contact_id`; trigger user owns `sender_seq`
- [ ] Shared reply (+1 seq) and shared full (+2 seq) flows
- [ ] Confirm UX before first shared send

---

## Phase post-v6d — Per-message transport badge UI (D051)

**Goal:** [DESIGN § Transport — `[post-v1]`](DESIGN.md#transport-provenance-d051).

**Depends on:** v4 `transport` column populated; libp2p direct path when available.

- [ ] E2E per-message Direct / Relay / Local indicator
- [ ] Read `transport` column; no inference from thread type alone

---

## Phase post-v6e — Relaxed ingest / continue anyway (optional)

**Goal:** [DESIGN § Relaxed ingest](DESIGN.md#post-v1-relaxed-ingest--continue-anyway-d046-extension) if product enables informed override.

**Depends on:** v6 integrity UX; explicit product decision (D046).

- [ ] `ingest_policy=relaxed`, `user_resolution=continue_anyway`, `trust_degraded` in `sync_state.state_json`
- [ ] Choice sheet secondary action after disclosure
- [ ] Hard crypto failures still have no override

---

## Cross-project — E2E crypto wire-up

**Not a chat-storage phase** — tracked in [e2e-message-crypto](../e2e-message-crypto/PHASES.md).

| Chat-storage gate | E2E phase | Work |
|-------------------|-----------|------|
| v2b channel split | c2 | `P2pMessagingService` encrypt/decrypt on `channel=e2e` |
| v6 envelope + seq | c2–c3 | AAD binds `sender_seq`; `ChatPayload` plaintext (E010) |
| v6 peer history (D060) | libp2p integration | `/pp-browser/chat-history/1.0.0` responder + requester |
| D038 integrity UX | c3 | PSK rotation + epoch bump transaction (D047) |

Ship public relay + SQLite storage without c2; E2E body crypto lands after v2b + v6 foundations.

---

## Deferred — cross-thread FTS search `[future]`

**Goal:** Agent/search across all threads without loading every transcript.

- [ ] Optional FTS5 virtual table in a thread or profile search DB (not `profile.db`)
- [ ] Internal API only until UI needs it

**Trigger:** search feature request or agent tool needs full-text recall.

---

## Cross-cutting tasks

- [ ] `SqliteThreadStore` unit tests (`profile.db`, per-thread db, clear, delete, outbox, **chat_targets**, **reconciliation**, size reject)
- [ ] Ingest tests: oversize envelope, remote content_rml stripped (D029–D030)
- [ ] Public vs E2E ingest path tests (D045)
- [ ] Agent tool docs if `list_conversations` must expose channel
- [ ] Fuzz/dedup: duplicate relay `message_id` ignored via per-thread store check
- [ ] `GetMessagesForContext` / compaction tests (D039–D040)
- [ ] Outbox/gap repair limit tests (D041)
- [ ] Relay fetch 403 for non-party chat target (D027)
- [ ] **`FetchChatTargetMessages`**: peer-direct then relay; empty gap close with D067 guard + late fill (D058–D061/D067)
- [ ] User-initiated sync + inbound find-only + compromised freeze tests (D059, D062, D068)

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
| 2026-06-30 | D067–D068: empty gap close guard + late fill; compromised outbox/sync freeze; epoch bump pending cancel; receive pipeline linearized |
