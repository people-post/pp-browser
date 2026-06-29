# Decisions log

Record significant choices here so future sessions (human or agent) do not re-litigate them. Format: **ID**, **date**, **decision**, **rationale**, **alternatives considered**.

---

## D001 — Storage backend (superseded by D028)

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — SQLite per thread from v2a (D028); no intermediate JSON message files.  
**Decision:** ~~JSON through v2–v4, SQLite optional v5~~ → **`SqliteThreadStore` from phase v2a** (D028). `JsonThreadStore` remains baseline code only until replaced; not extended for new layouts.  
**Rationale:** Per-thread directory maps to `thread.db`; seq-range queries and durable outbox need indexes; D016 allows wipe without JSON migration path.  
**Alternatives:** JSON dirs then SQLite at v6 (rejected); single profile-wide `threads.db`.

---

## D002 — Thread index + per-thread directory (superseded on-disk detail by D025, D035)

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — per-thread directory layout (D025); sidebar catalog moved to `profile.db` `threads` table (D035).  
**Decision:** ~~Retain `threads/index.json` for sidebar metadata~~ → **`profile.db` `threads` table** (list cache; D035). Each thread owns a **directory** `{thread_id}/` with **`thread.db`** — not a single flat `{thread_id}.json`.  
**Rationale:** Sidecar memory, atomic writes per artifact, room for attachments later; profile-level SQLite catalog replaces JSON dual-write.  
**Alternatives:** Single file per thread (original D002); monolithic database; keep `index.json` (rejected — D035).

---

## D003 — Three-layer clear semantics

**Date:** 2026-06-27  
**Decision:** Separate user actions for (1) clear visible transcript, (2) forget AI memory, (3) delete conversation / new chat. Transcript and memory can be cleared independently.  
**Rationale:** Users expect “clear chat” ≠ “make AI forget”; long threads need memory that survives UI scroll trim.  
**Alternatives:** Single “clear” that wipes everything always.

---

## D004 — Separate threads for public vs E2E per contact

**Date:** 2026-06-27  
**Decision:** Thread identity for direct chats is `(contact_id, channel)`, not contact alone. Two thread records (and files) for the same person when both modes exist.  
**Rationale:** Different crypto, transport, privacy UI, and AI memory boundaries; avoids mode-switch bugs in one transcript.  
**Alternatives:** One thread with per-message encryption flags; single thread with “mode switch” rewriting history.

---

## D005 — UUID message IDs + payload rows (see D026)

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — unified `ChatPayload` (D026).  
**Decision:** Every message has a stable UUID. Reactions, cards, and txs are **separate messages** with their own id and `content_type` — not mutations of the target row. Annotations reference `target_message_id` in `payload`.  
**Rationale:** Relay dedup uses `message_id`; append-only simplifies sync; LLM excludes non-text types by default.  
**Alternatives:** Embedded likes array; positional indices as IDs.

---

## D006 — Transport field distinct from delivery

**Date:** 2026-06-27  
**Decision:** Add `transport` (local / relay / direct) separate from `delivery` (pending / relayed / failed). UI in E2E mode shows transport; delivery shows send pipeline state.  
**Rationale:** A message can be `delivery=relayed` via `transport=direct` or `transport=relay`; users in private mode need to know fallback path.  
**Alternatives:** Overload `delivery` enum; infer transport in UI from thread type only.

---

## D007 — Converge on ThreadMessage, deprecate dual Conversation path

**Date:** 2026-06-27  
**Decision:** Target state uses `IThreadStore` / `ThreadMessage` as the only durable transcript for AI and P2P. In-memory `Conversation` becomes an implementation detail or is removed from hot paths when messaging is enabled.  
**Rationale:** [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) already describes conversation-first design; dual models caused persistence gaps for AI home.  
**Alternatives:** Persist `Conversation` separately and sync to threads; SQLite-only unified table without JSON migration path.

---

## D008 — Sender-assigned `sender_seq` alongside UUID

**Date:** 2026-06-27  
**Decision:** Peer-visible direct `content` messages carry a monotonic **`sender_seq` (uint64) per sender per chat target**, in addition to UUID `message_id`. Seq is **per-sender**, not one global thread counter — in 1:1 chat each participant has an independent stream. UUID remains the sole message identity for dedup and annotations; seq is metadata for ordering and gap detection.  
**Rationale:** UUID dedup prevents duplicates but cannot detect missing messages; gap detection is critical for private chat reliability. Per-sender seq scales to group chat later.  
**Alternatives:** Relay-assigned global log index; positional indices as IDs (rejected in D005); timestamp-only ordering.

---

## D009 — Two sync modes for E2E (tail + gap repair)

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — history backfill deferred (D052); floor rules unchanged (D037).  
**Decision:** **E2E** P2P history sync uses two active modes: **(1) tail sync** — on open, reconnect, or new device, fetch latest N (default 50) messages per peer; **(2) gap repair** — automatic backfill when a hole appears in the contiguous tail (have seq N, receive N+2+), not gated on scroll. **Scroll-driven history backfill** is **deferred** (D052) — v1 scroll-up uses local `GetMessagesPage` only. Bootstrap/tail ingest on an empty store does **not** treat a high incoming seq as a gap. **Public relay** uses poll + local pages only — no seq sync modes (D045).  
**Rationale:** Tail + gap covers live private-chat reliability; scroll backfill is lower pre-launch value.  
**Alternatives:** Three modes in v1 (original D009); poll cursor only.

---

## D010 — Seq lifecycle on chat target

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — below-floor ingest after clear is sync exclusion + silent discard (D037), not compromised.  
**Decision:** `next_outgoing_seq` and `session_epoch` are keyed to **chat target `(contact_id, channel)`**, surviving thread delete/recreate and **clear visible history** (seq is not reset on clear). On clear, receiver sets `history_floor_seq[peer][epoch]` to the max contiguous seq at clear time; relay-visible seq at or below the floor in the same epoch is **excluded from sync** (D037) — silent discard, not compromised. Failed sends retry with the **same `message_id` and `sender_seq`**. New device / full reset requires **epoch bump** (D014). `uint64` overflow is accepted as out of scope.  
**Rationale:** Seq represents the long-lived conversation with a contact, not a local transcript snapshot; idempotent retries must not bump seq; clear history is a local display choice, not a protocol reset; floor marks a one-way local cutoff without protocol reset.  
**Alternatives:** Reset seq on clear; assign seq only after successful relay; thread-scoped counters; below-floor → compromised (superseded by D037).

---

## D011 — Session compromise on conflicting seq

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — recovery UX per D038; no continue-anyway (D046).  
**Decision:** If the same `(sender, session_epoch, sender_seq)` is received with a **different `message_id`**, treat as **session integrity failure** (E2E only, D045): **pause ingest and outbound**, record an integrity incident, show a choice sheet (D038, D046). **Recommended** recovery: rotate E2E keys + bump `session_epoch`. User may **not** choose continue-anyway in v1 (D046). Same `(message_id, sender_seq)` duplicates are benign (UUID dedup). Envelope signature must bind `sender_seq` and `session_epoch`.  
**Rationale:** Under encryption, conflicting seq implies replay, split-brain, or attack; silent merge by default would break trust. Informed user override is allowed when risks are disclosed.  
**Alternatives:** Last-write-wins without disclosure (rejected); ignore conflict silently (rejected); log only (rejected).

---

## D012 — `@ai` in direct threads (local only v1)

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — shared modes deferred; see [PHASES.md](PHASES.md) § Deferred.  
**Decision:** v1 ships **local `@ai` only**. **`[post-v1]`:** `@ai+` / `@ai++` — [DESIGN.md § `@ai` in direct threads](DESIGN.md#ai-in-direct-threads-d012).  
**Rationale:** Matches shipped behavior; shared modes deferred.  
**Alternatives:** Three modes in v1 (original D012).

---

## D013 — Strict ingest (E2E direct only)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — scope **E2E only** (D045); public relay UUID+timestamp; no relaxed ingest (D046).  
**Decision:** In **`e2e` direct threads**, the receiver uses D013: normal, gap, soft compromised, hard reject. Below-floor → D037 silent discard. Soft failures → pause + choice sheet (D038, D046). **`public_relay`** direct: signature verify + participant check + UUID dedup — **no seq classifier**. Sync watermarks keyed by **`(peer, session_epoch)`** on E2E only.  
**Rationale:** Seq integrity where crypto matters; UUID dedup sufficient for public relay v1.  
**Alternatives:** Strict ingest on both channels (original D013/D018).

---

## D014 — Peer reset requires `session_epoch` bump

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — no `epoch_start` row; epoch bump transaction (D047).  
**Decision:** Full peer reset **must bump `session_epoch`** via epoch bump transaction. Reset `next_outgoing_seq = 1`. **No `epoch_start` system message.** Restored backup with same `profile.db` + crypto sessions continues existing epoch.  
**Rationale:** Seq restart must be explicit and scoped; epoch is the namespace boundary; avoids ambiguous “fresh” traffic in an old epoch.  
**Alternatives:** Ad-hoc first-message flag without epoch; allow seq rewind on reinstall; reset seq on clear history.

---

## D015 — Single active sender per identity (v1)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — recovery UX per D038.  
**Decision:** v1 assumes **one active sending client per profile identity** per **E2E** chat target (D045). Running the same identity on two devices without coordination is unsupported — conflicting `sender_seq` triggers a **soft integrity failure** (D011): pause + choice sheet (D038), not silent merge. Document in settings/help; no device-scoped sub-seq or relay seq lease in v1.  
**Rationale:** Per-chat-target seq is simple and sufficient pre-launch; multi-device coordination is a large protocol surface.  
**Alternatives:** Device-scoped seq in envelope; central seq lease via relay; per-device PSK.

---

## D016 — No legacy migration

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — includes legacy relay envelopes with `thread_id` (D056).  
**Decision:** **No in-place upgrade** from pre-v2b/v6 on-disk thread JSON or **legacy relay wire** (envelopes with `thread_id`, old AAD with `thread_id`). Schema / protocol bumps may require deleting `{data_dir}/profiles/{id}/threads/` (acceptable — no production users yet). **Single parser** for wire + AAD — no dual-version support.  
**Rationale:** Avoid migration and compatibility code while the model is still evolving; devs wipe local data on breaking bumps.  
**Alternatives:** Backfill `sender_seq` on old messages; accept legacy envelopes alongside new (rejected).

---

## D017 — Durable outbox from thread store

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `profile.db` holds `threads` catalog + `outbox` (D035).  
**Decision:** Pending/failed `relay_visible` messages are the **durable outbox** — persisted in `thread.db` `messages` table before send. **`profile.db` `outbox` table** indexes pending/failed rows for O(1) startup scan (D028). In-memory retry queue is a performance layer only. Retries reuse same `(message_id, sender_seq)`. Relay idempotent on `message_id`. Message-id dedup is **per-thread** in `thread.db` (D034), not in `profile.db`. Sidebar catalog is separate (`threads` table, D035).  
**Rationale:** Restart must not drop unsent messages; `profile.db` avoids opening every `thread.db` on startup.  
**Alternatives:** Full scan all thread DBs on startup; separate outbox file only.

---

## D018 — Ingest scope by channel (supersedes “strict on public”)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — public relay UUID+timestamp (D045); E2E keeps D013.  
**Decision:** **E2E** direct: full D013 + D038 (recommended recovery only, D046). **Public relay** direct: UUID dedup, participant validation, signature verify — no seq sync or compromise UX.  
**Rationale:** Channel-appropriate integrity cost.  
**Alternatives:** Strict ingest on both (original D018).

---

## D019 — Transcript display ordering

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — UI sort via `display_order` (D054); seq/timestamp roles split.  
**Decision:** **UI / pagination:** `display_order ASC` on every message (D054). **`BuildDisplayRows`** and **`GetMessagesPage`** use `display_order` only. **E2E sync:** `(session_epoch, sender_contact_id, sender_seq)` for ingest and `GetMessagesBySeqRange` — not UI sort. **Public relay:** UUID dedup at ingest; `display_order` at persist. `timestamp` is metadata, not transcript sort.  
**Rationale:** Per-sender seq sort does not interleave 1:1 turns; timestamp pagination breaks E2E; one column unifies AI, public, E2E, and local `@ai` rows.  
**Alternatives:** Sort UI by `(sender, seq)` (rejected); `before_timestamp` pagination (rejected — D054).

---

## D020 — Reorder window before gap declaration (E2E)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — implemented via `ReplayWindow` helper; D013 authoritative (D045).  
**Decision:** E2E only. Hold out-of-order messages in **`kReorderWindow = 32`** slots via `base/crypto` **`ReplayWindow`**; feature-layer **D013 classifier is authoritative** — helper does not override compromise policy.  
**Rationale:** Benign reorder during multi-path delivery should not false-alarm.  
**Alternatives:** Inline buffer only in feature layer.

---

## D021 — `sender_contact_id` and `route` on relay envelope

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `route` object; no `thread_id` (D056).  
**Decision:** Relay envelope includes **`sender_contact_id`** and **`route`** (`kind` + `channel` for direct). **No `thread_id`** on wire. Inbound direct routing: `ChatTargetKey { sender_contact_id, route.channel }` → local `local_thread_id`. Do not infer sender from local thread metadata.  
**Rationale:** Local thread ids differ per device; shared routing key is the chat target. `route` extensible to `group_id` later.  
**Alternatives:** Shared wire `thread_id` (D053 — superseded); infer sender from participants[0] (rejected).

---

## D022 — Receive pipeline step order

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — resolve `ChatTargetKey` before dedup (D056); reject legacy `thread_id`.  
**Decision:** Ingest order: envelope size → **reject `thread_id` if present** → signature verify → parse `route` → **resolve local thread via `ChatTargetKey`** → per-thread UUID dedup → participant check → decrypt (e2e) → parse `ChatPayload` → history floor (D037) → D013 classifier → persist. See DESIGN.md § Receive pipeline. **Superseded in detail by D033** (plaintext size after decrypt).  
**Rationale:** Dedup and persist require local `thread_id`; wire carries no thread id.  
**Alternatives:** Dedup before routing using envelope `thread_id` (superseded).

---

## D023 — Sidebar channel badge (not grouped sections)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — flat list + per-row badge replaces collapsible groups.  
**Decision:** Sidebar **flat list** sorted by `updated_at`; direct rows show **Public** / **Private** channel badge. Same contact may appear twice. No collapsible section headers in v1.  
**Rationale:** Channel distinction without grouped-list UI complexity.  
**Alternatives:** Collapsible groups (original D023).

---

## D024 — Clear history as two-action choice sheet

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — two actions + optional forget-AI checkbox.  
**Decision:** **Clear messages** (optional *Also forget what AI learned* checkbox) · **Delete conversation**. Separate **Forget what AI learned** menu item. P2P disclosure on clear.  
**Rationale:** Simpler UX; same D003 semantics.  
**Alternatives:** Three-level sheet (original D024).

---

## D025 — Per-thread directory (superseded file layout by D028, D035)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `thread.db` replaces JSON files (D028); sidebar catalog in `profile.db` (D035).  
**Decision:** On-disk layout uses **one directory per thread** under `threads/{thread_id}/` containing **`thread.db`**. Profile-level **`threads/profile.db`** holds **`threads`** catalog + **`outbox`** + **`chat_targets`** (D017, D034, D047). **No `index.json`.** Delete thread = `profile.db` transaction then remove `{thread_id}/` directory. **No migration** from legacy JSON (D016).  
**Rationale:** Directory delete semantics preserved; SQLite gives append, seq-range queries, and row-level delivery updates; single profile DB for list + outbox.  
**Alternatives:** `messages.json` + `memory.json` sidecars (rejected — skip intermediate JSON stage); `index.json` sidebar (rejected — D035).

---

## D026 — Unified `ChatPayload` message format

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — v1 types text + system only (D050).  
**Decision:** One wire/disk payload shape. **v1 validator accepts `text` and `system` only**; reject unknown `content_type` on inbound relay. **`[post-v1]`** rich types — [DESIGN.md § ChatPayload](DESIGN.md#chatpayload-unified-message-body--d026).  
**Rationale:** One parser path; defer UI/protocol surface until needed.  
**Alternatives:** All types in v1 (original D026).

---

## D027 — Relay chat-target messages API (seq backfill, E2E)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `GET /v1/chat-targets/messages`; no `thread_id` (D056).  
**Decision:** Relay exposes seq-scoped fetch by **`ChatTargetKey`** (`peer_contact_id` + `channel` query params). **Authorization:** caller must be a party to that chat target; else **403**. Relay indexes by recipient inbox + sender + channel, not client `thread_id`. Send idempotent on `message_id`. **Reject** POST bodies with `thread_id`.  
**Rationale:** Peers use different local thread ids; backfill keys must match wire routing.  
**Alternatives:** `GET /v1/threads/{thread_id}/messages` (superseded).

---

## D028 — SQLite per thread + `profile.db` from v2a

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — sidebar catalog in `profile.db` `threads` table (D035); no `index.json`.  
**Decision:** Phase **v2a** ships **`SqliteThreadStore`** (not an intermediate JSON-per-dir stage). Layout:

- `threads/profile.db` — **`threads`** catalog (D035) + **`outbox`** (D017) + **`chat_targets`** (`ChatTargetKey` PK, `local_thread_id`, seq, epoch — D047, D056)
- `threads/{thread_id}/thread.db` — `messages` (+ `display_order`, D054), `memory`, `sync_state`; **`messages.id`** is the per-thread dedup key (D034)

Vendor SQLite in `pp_base` (not libp2p fork). `IThreadStore` is the only feature seam; `JsonThreadStore` deprecated after cutover. **No JSON thread files** in target layout. Wipe legacy `threads/index.json` and `threads/*.json` on upgrade (D016).  
**Rationale:** v6 seq sync, durable outbox, and `ChatPayload` append path need indexed storage; per-thread DB preserves delete/clear isolation; `profile.db` avoids scanning every `thread.db` for pending sends on startup and for sidebar sort. Per-thread dedup uses the existing `messages` PK — no separate global `message_ids` table.  
**Alternatives:** JSON dirs in v2a then SQLite at v6; single monolithic `threads.db`; JSON `memory.json` sidecar (merged into `thread.db`); keep `index.json` (rejected — D035).

---

## D029 — Chat resource bounds (size & volume)

**Date:** 2026-06-29  
**Decision:** Enforce explicit caps on chat wire, storage, and relay traffic. Constants in `MessagingLimits.h` (name TBD); reject at compose, send, and ingest.

| Limit | Value | Applies to |
|-------|-------|------------|
| `kMaxComposeTextBytes` | **64 KiB** | User composer `text` |
| `kMaxChatPayloadJsonBytes` | **64 KiB** | Serialized `ChatPayload` on `public_relay` |
| `kMaxE2ePlaintextBytes` | **128 KiB** | AEAD plaintext JSON (E010); checked before/after decrypt |
| `kMaxRelayEnvelopeJsonBytes` | **256 KiB** | Full signed POST body |
| `kMaxContentRmlBytes` | **256 KiB** | **Local-only** assistant RML on disk (not accepted from wire) |
| `kMaxUserPayloadBytes` | **64 KiB** | Persisted `user_payload` on AI thread rows (aligns with platform-safety-limits) |
| `kMaxChatActionsPerMessage` | **32** | `chat_actions` array |
| `kMaxPollBatchMessages` | **100** | Per inbox poll or relay fetch response |
| `kMaxRetryQueueItems` | **500** | In-memory + `profile.db` outbox rescan cap |
| `kMaxOpenThreadDbs` | **16** | LRU of open `thread.db` handles |
| `kMaxDisplayPageMessages` | **100** | Default UI transcript window |

No hard cap on messages per thread or threads per profile in v1 — monitor via dev logging; revisit if needed.  
**Rationale:** Prevents OOM, relay abuse, and accidental paste bombs; aligns with SQLite row sizes.  
**Alternatives:** No limits; 4 KiB SMS-style cap; single 1 MiB blob for everything.

---

## D030 — Untrusted remote content: no wire `content_rml`

**Date:** 2026-06-29  
**Decision:** **P2P / relay ingest must not persist or render `content_rml` from envelopes.** Peers send **`ChatPayload` only** (`text`, cards, annotations, etc.). RML is generated **locally** for AI assistant rows and untrusted peer text is escaped to plain bubbles. Strip or ignore `body.content_rml` on poll if present.  
**Rationale:** Remote RML is equivalent to arbitrary markup injection; today `BuildMessageRml` renders peer `content_rml` without escaping.  
**Alternatives:** Sanitize remote RML subset; allow signed rich content later.

---

## D031 — Windowed transcript UI + `GetMessagesPage`

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `before_display_order` cursor (D054).  
**Decision:** UI loads **pages** of messages, not full thread history. `IThreadStore::GetMessagesPage(thread_id, before_display_order, limit)` default **100**; `before_display_order` null = newest page; scroll-up passes oldest loaded row’s `display_order` (D054). `BuildDisplayRows` operates on the loaded window sorted by `display_order`. **Agent turns** use `GetMessagesForContext` (D039), not full-thread `GetMessages`. Retain `GetMessages` for tests, export, and dev tools only.  
**Rationale:** Avoid O(n) memory and RML build per frame on long threads; unified cursor for all channel types.  
**Alternatives:** Virtualized list with full load; `before_timestamp` cursor (rejected — D054).

---

## D032 — Relay poll backoff and batch limits

**Date:** 2026-06-29  
**Decision:** Do **not** call `PollAndMerge` every UI frame. **Foreground:** poll at most once per **2 s** (configurable); **background:** pause or **30 s**. Reject inbox responses with more than **`kMaxPollBatchMessages`** (D029) without processing. Align HTTP relay contract (D027) with `kMaxRelayEnvelopeJsonBytes`.  
**Rationale:** Today `ChatController::Update` polls every tick; wastes IO and merges unbounded batches.  
**Alternatives:** Push-only from relay; 5 s MCP throttle only.

---

## D033 — Ingest pipeline: size check before parse

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — aligns with D022/D056 receive order.  
**Decision:** Inbound order per D022: envelope size → reject legacy `thread_id` → signature → `route` → resolve `ChatTargetKey` → dedup → participant check → decrypt (e2e) → **plaintext size** → parse `ChatPayload` → history floor (D037) → D013 → persist. Reject oversize before `nlohmann::parse` on untrusted input.  
**Rationale:** JSON bombs and huge ciphertext must fail closed without full parse.  
**Alternatives:** Parse then check (rejected).

---

## D034 — Per-thread `message_id` dedup (no `message_ids` in `profile.db`)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `profile.db` also holds `threads` catalog (D035); still no `message_ids` table.  
**Decision:** Ingest idempotency and relay redelivery dedup are **per local thread**: resolve **`ChatTargetKey` → `local_thread_id`**, then `HasMessageId(local_thread_id, message_id)` on `thread.db` `messages.id` PK. **`profile.db` holds `threads` + `outbox`** — no `message_ids` table.  
**Rationale:** Wire has no `thread_id`; routing must happen before per-thread dedup. O(threads) catalog, not O(messages).  
**Alternatives:** Global `message_ids` in `profile.db` (rejected); dedup using envelope `thread_id` (superseded).

---

## D035 — Sidebar catalog in `profile.db`; lazy truth-check on list

**Date:** 2026-06-29  
**Decision:** Drop **`threads/index.json`**. Sidebar list metadata lives in **`profile.db` → `threads` table** (kind, channel, title, participant_contact_ids, cached preview, updated_at, unread_count). **`thread.db` is authoritative** for thread existence and message-derived fields; the `profile.db` catalog row is a **denormalized list cache**, not a second source of truth.

**Write path (balanced):**

- `AppendMessage` / `UpdateMessage`: persist in `thread.db` first; then `UPDATE threads SET updated_at=?, unread_count=?` in `profile.db` (sort/unread stay fresh).
- **Preview** is not required on every append — refresh from `thread.db` when the row is **verified on list** (see below). Optionally update preview synchronously for the **active** thread only.

**List / repair (`ListThreads`):**

1. `SELECT` from `profile.db` `threads` ordered by `updated_at` (one profile DB read).
2. For **visible rows only** (current sidebar viewport / page slice — not all threads): if `{thread_id}/thread.db` is missing → delete `threads` + `outbox` rows for that id; else run `SELECT text, timestamp FROM messages ORDER BY timestamp DESC LIMIT 1` and update cached preview/`updated_at` if mismatched.
3. `FindOrCreateDirectThread(ChatTargetKey)` looks up **`chat_targets`**; catalog via **`threads.direct_peer_contact_id`** + `channel` (D055).

**Profile open (once):** `readdir` `threads/*/` — directories with `thread.db` but no `threads` row → insert stub catalog row (repairs crash-after-DB-create).

**Delete thread:** single `profile.db` transaction — `DELETE FROM threads` + `DELETE FROM outbox WHERE thread_id=?` — then remove `{thread_id}/` directory.

**Clear messages:** keep `threads` row; visible verify shows empty preview when transcript is wiped.

**Rationale:** Avoids JSON dual-write and crash corruption; keeps fast sidebar sort in one SQLite file; lazy verify limits cost to ~viewport-sized `thread.db` opens per list refresh, not N threads. Correctness over eager full scan.  
**Alternatives:** Keep `index.json` (rejected); no catalog — scan every `thread.db` on list (rejected); eager full truth-check on every `ListThreads` (rejected); preview always duplicated on every append without verify (rejected).

---

## D036 — Rename `registry.db` → `profile.db`

**Date:** 2026-06-29  
**Decision:** Profile-level SQLite file is **`threads/profile.db`** (not `registry.db`). Holds **`threads`** catalog + **`outbox`** index (D035, D017). Name reflects scope: profile-scoped metadata, not only an outbox registry.  
**Rationale:** After D035 the file holds sidebar catalog and outbox; `registry.db` was misleading. `profile.db` pairs naturally with per-profile `identity.json` / `contacts.json` layout.  
**Alternatives:** Keep `registry.db` name; move file to `{profile_id}/profile.db` outside `threads/` (deferred — path stays under `threads/` for v2a).

---

## D037 — Clear history floor: sync exclusion + silent discard

**Date:** 2026-06-29  
**Decision:** After **clear visible history** (D024), receiver **`DELETE FROM messages`** (transcript and per-thread dedup surface wiped — D034) and sets **`history_floor_seq[peer][epoch]`** to the max contiguous peer seq at clear time. For the same epoch, any inbound or relay-fetched message with **`sender_seq ≤ history_floor_seq`** is a **sync exclusion zone**, not an integrity failure: **silent discard** — do not persist, backfill, show, or bump unread; do **not** enter `sync_state=compromised`. Applies to **all paths**: poll, direct, tail sync, gap-repair responses. Only **`sender_seq > floor`** is eligible for normal D013 ingest. **No scroll resurrection** of cleared history in the same epoch — user cannot re-sync seq at or below floor. Clients must clamp relay fetch when floor is set: use **`min_sender_seq = floor + 1`** on tail and gap requests; discard any below-floor rows in responses without compromising. **No post-clear message-id tombstone** in `profile.db` — dedup stays transcript-scoped (`messages.id`); below-floor traffic is dropped by seq before dedup matters. Full conversation restart (new device, explicit start-over) uses **epoch bump** (D014), not clear.  
**Rationale:** Clear is a one-way local cutoff, not a protocol reset; honest senders continue at seq N+1; relay poll redelivery of pre-clear messages must not trigger compromise UX; floor remembers seq progress without retaining transcript rows or a separate registry.  
**Alternatives:** Below-floor → compromised (original D013 wording); soft-clear tombstone rows with content hydration on scroll; permanent message-id registry after clear.

---

## D038 — E2E integrity recovery (no continue-anyway)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — superseded relaxed ingest (D046); E2E-only scope (D045).  
**Decision:** On E2E soft integrity failure: **pause ingest/outbound**, append incident (ring buffer D049), show choice sheet with disclosure. User picks **`rotate_psk`** (start new secure chat) or **`pause_only`**. **No `continue_anyway`**, no `ingest_policy=relaxed`, no `trust_degraded` in v1. Persist in `sync_state.state_json`:

| Field | Values |
|-------|--------|
| `user_resolution` | `null` \| `rotate_psk` \| `pause_only` |
| `user_acknowledged_at` | unix ms |
| `integrity_incidents[]` | ring buffer, max **10** (D049) |

Hard failures: pause + **Pause only** until delete thread or key rotation.  
**Rationale:** Strict recovery avoids dual classifier and ambiguous outbound interop.  
**Alternatives:** Continue anyway with relaxed ingest (original D038). Full rules: [DESIGN.md § Relaxed ingest](DESIGN.md#post-v1-relaxed-ingest--continue-anyway-d046-extension).

---

## D039 — Agent context: tail read + memory summary (no full-thread load)

**Date:** 2026-06-29  
**Decision:** `AgentSession` and `ThreadContextPolicy` must **not** call full-thread `GetMessages` on the hot path. Add `IThreadStore::GetMessagesForContext(thread_id, ContextBudget)`:

1. Load **`GetThreadMemory`** summary when present (v3 `memory` table).
2. Fetch a **tail slice** by `display_order DESC` of `content_type=text` (+ selected `system`) until `max_turn_pairs` / `max_recent_chars` from `ContextBudget` is satisfied — indexed limit, not full transcript scan.
3. Return messages in chronological order for `ThreadContextPolicy::Build`.

`GetMessages` remains for unit tests, export, and migration; mark deprecated in `IThreadStore` comments for feature code.  
**Rationale:** D031 windowing fixes UI only; loading 10k rows per LLM turn is O(n) IO despite in-memory trim. Summary + tail matches the three-layer model (transcript vs context vs memory).  
**Alternatives:** Keep full `GetMessages` + trim in policy (rejected); always load exactly `max_turn_pairs * 2` rows without summary injection.

---

## D040 — AI compaction triggers and summary bounds (v3)

**Date:** 2026-06-29  
**Decision:** `ICompactionService` runs when a thread’s **text turn count** (user + assistant `content_type=text` rows) exceeds **`kCompactionTurnThreshold = 20`** since the last summary version. Constants in `MessagingLimits.h` (or shared with `ContextBudget`):

| Constant | Value | Notes |
|----------|-------|-------|
| `kCompactionTurnThreshold` | **20** | Turns since last summary before compaction eligible |
| `kMaxSummaryBytes` | **8 KiB** | Persisted `ConversationSummary.text` on disk |
| `kCompactionMinTurnsKept` | **6** | Tail turns always kept verbatim in context after summary |

**Trigger:** **async after turn completes** — enqueue compaction job; do not block composer send or LLM response delivery. On failure, log and retry next eligible turn; never delete transcript rows. Summary `version` increments on successful write; persist **`compacted_through_display_order`** in `memory` summary JSON so eligibility does not require full-thread scan. Wire `max_summary_chars` in `ContextBudget` to **`kMaxSummaryBytes`** at runtime (chars ≈ bytes for UTF-8 summary text).  
**Rationale:** v3 needs explicit bounds so memory table and LLM injection do not grow without limit; async avoids UI stalls.  
**Alternatives:** Inline compaction blocking the turn (rejected); no summary size cap (rejected); compact on char count only (deferred).

---

## D041 — Outbox retry and gap repair numeric limits

**Date:** 2026-06-29  
**Decision:** Add explicit P2P retry/repair caps in `MessagingLimits.h`:

| Constant | Value | Applies to |
|----------|-------|------------|
| `kMaxOutboxRetryAttempts` | **12** | Per `message_id` durable outbox resend attempts (exponential backoff) |
| `kMaxGapRepairRounds` | **5** | Consecutive repair cycles per `(peer, epoch)` gap before soft compromised (D038) |
| `kMaxGapRepairSeqSpan` | **500** | Max `max_sender_seq - min_sender_seq + 1` per single repair fetch |

After **`kMaxOutboxRetryAttempts`**: set `delivery=failed`, keep row, show persistent send-failure affordance; user may retry manually (resets attempt counter). After **`kMaxGapRepairRounds`**: pause + integrity choice sheet (D038). If startup `ListPendingOutbox` returns more than **`kMaxRetryQueueItems`** (D029), process first 500 in **`outbox.updated_at ASC`** order and log warning — do not drop rows.  
**Rationale:** “Cap attempts” and “repair exhaustion” in DESIGN were qualitative; implementers need constants.  
**Alternatives:** Unlimited retries; immediate compromised on first repair miss.

---

## D042 — Annotation volume cap per target message

**Date:** 2026-06-29  
**Decision:** Cap **`kMaxAnnotationsPerTarget = 32`** — max annotation rows (`content_type=annotation`) referencing the same `target_message_id` in one thread. Reject compose/send locally; reject ingest above cap (count existing rows for target before persist). Applies to reactions, edits, and future annotation types — append-only rows (D026), each state change keeps its own `message_id`.  
**Rationale:** Prevents reaction/annotation spam and unbounded `BuildDisplayRows` merge work; complements `kMaxChatActionsPerMessage` on assistant rows.  
**Alternatives:** Per-sender rate limit only (deferred); unlimited annotations (rejected); mutate target row in place (rejected — D026).

---

## D043 — Orphan annotations (missing target)

**Date:** 2026-06-29  
**Decision:** When `target_message_id` is absent from the thread transcript (never received, cleared, or deleted): **accept and persist** the annotation row; **do not** fail ingest or compromise. UI renders as a **standalone row** with orphaned badge (e.g. “Reply to earlier message”) — not inline on a missing bubble. `BuildDisplayRows` skips merge onto missing targets. LLM context excludes orphan annotations (same as other annotations).  
**Rationale:** Clear history (D037) and partial sync leave valid annotation envelopes; failing closed would alarm users; hiding would lose audit trail.  
**Alternatives:** Reject ingest when target missing (rejected); silently drop annotation (rejected).

---

## D044 — SQLite operational policy (`thread.db` / `profile.db`)

**Date:** 2026-06-29  
**Decision:** `SqliteThreadStore` uses:

- **`PRAGMA journal_mode=WAL`** per connection.
- **Single writer mutex** per `thread.db` and one for `profile.db` — all `AppendMessage` / `ClearMessages` / catalog updates serialize; concurrent readers allowed on WAL.
- **Lock order** when both DBs are touched: **`profile.db` first**, then `thread.db`; never hold `thread.db` while acquiring `profile.db`.
- After **`ClearMessages`** bulk delete: run **`PRAGMA wal_checkpoint(PASSIVE)`** on that `thread.db` (best-effort; do not block UI).
- **No automatic `VACUUM`** in v1; monitor `thread.db` file size via dev logging. Revisit if clear/delete leaves large sparse files.

No hard max file size in v1.  
**Rationale:** WAL + mutex matches lazy-open multi-threaded UI + background poll; checkpoint reduces WAL growth after large clears.  
**Alternatives:** DELETE journal mode (rejected); auto-VACUUM on every clear (too slow).

---

## D045 — E2E-only `sender_seq` and strict ingest

**Date:** 2026-06-29  
**Decision:** **`sender_seq` / `session_epoch` on the wire apply to `e2e` direct threads only.** Public relay uses UUID dedup + timestamp display order; no seq assignment, gap repair, or D013 classifier on public channel.  
**Rationale:** Seq integrity buys most for encrypted private delivery; public content is relay-visible.  
**Alternatives:** Seq on both channels (original D008/D018).

---

## D046 — No relaxed ingest / continue-anyway in v1

**Date:** 2026-06-29  
**Decision:** Supersedes relaxed-ingest portions of D038. On E2E soft compromised, user picks **Start new secure chat** or **Pause only** — no `continue_anyway`, no `ingest_policy=relaxed`, no `trust_degraded`.  
**Rationale:** Avoids dual classifier and ambiguous cross-peer policy.  
**Alternatives:** Informed continue-anyway (original D038).

---

## D047 — `chat_targets` table in `profile.db`

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `local_thread_id`; `ChatTargetKey` (D056).  
**Decision:** **`chat_targets`** PK = **`(contact_id, channel)`** (`ChatTargetKey`). Columns: **`local_thread_id`** (current on-disk shell; **not on wire**), **`next_outgoing_seq`**, **`session_epoch`**. Updated under same writer mutex as `outbox`. **Delete direct conversation** removes shell but **keeps** `chat_targets` (seq/epoch). Shell recreate may allocate **new** `local_thread_id`.  
**Rationale:** Seq/epoch are per logical chat target; local storage ids are device-private.  
**Alternatives:** Wire-stable `thread_id` (D053 — superseded).

---

## D048 — No encryption at rest for thread DBs (v1)

**Date:** 2026-06-29  
**Decision:** `thread.db` and `profile.db` are **plaintext SQLite**. E2E confidentiality is wire-only. SQLCipher / OS keychain for transcripts deferred.  
**Rationale:** Explicit assumption; PSK-at-rest is separate (e2e E008).  
**Alternatives:** Encrypt all thread DBs in v2a.

---

## D049 — Cap integrity incidents

**Date:** 2026-06-29  
**Decision:** **`kMaxIntegrityIncidents = 10`** — ring buffer in `sync_state.state_json`; drop oldest when full.  
**Rationale:** Prevents unbounded JSON growth.  
**Alternatives:** Unlimited append-only log.

---

## D050 — ChatPayload v1: text + system only

**Date:** 2026-06-29  
**Decision:** v1 validator accepts **`text`** and **`system`** only. Defer **`annotation`**, **`contact_card`**, **`crypto_tx`** — enum reserved, reject on ingest.  
**Rationale:** Rich types add UI, merge logic, and protocol surface with low pre-launch value.  
**Alternatives:** All D026 types in v4 (original plan).

---

## D051 — Defer per-message transport badge UI

**Date:** 2026-06-29  
**Decision:** Persist `transport` column at send/receive; **do not ship per-message relay/direct badge UI** until libp2p direct messaging exists. E2E thread shell styling remains.  
**Rationale:** All traffic is relay today; badge is noise until direct path exists.  
**Alternatives:** Ship badge in v4 (original plan).

---

## D052 — Defer scroll-driven history backfill

**Date:** 2026-06-29  
**Decision:** **No relay fetch when user scrolls to top** in v1. Scroll-up uses **`GetMessagesPage`** on local transcript only. E2E active sync: **tail + gap repair** only (amends D009). **`[post-v1]`** history backfill: [DESIGN.md § P2P sync](DESIGN.md#p2p-sync-e2e-only--d045).  
**Rationale:** Large subsystem; tail+gap covers live reliability.  
**Alternatives:** Three sync modes in v1 (original D009).

---

## D053 — Stable `thread_id` per direct chat target (superseded by D056)

**Date:** 2026-06-29  
**Superseded:** 2026-06-29 — D056 (local `thread_id`; wire uses `ChatTargetKey`).  
**Decision:** ~~Wire-shared stable `thread_id`~~ — do not implement.  
**Rationale:** Superseded — peers keep private local ids; routing via `sender_contact_id` + `route.channel`.

---

## D054 — `display_order` for UI sort and pagination

**Date:** 2026-06-29  
**Decision:** Every `messages` row has **`display_order INTEGER NOT NULL`**, assigned in **`AppendMessage`** per DESIGN § Display order assignment. **UI sort** and **`GetMessagesPage(before_display_order)`** use this column only. **`sender_seq`** remains for E2E sync/ingest only (D045). Default append: `max+1`; gap repair: insert between seq neighbors (renumber tail if needed).  
**Rationale:** Unifies AI, public, E2E, and local `@ai` transcript ordering; avoids channel-specific pagination cursors.  
**Alternatives:** `before_timestamp` pagination; runtime merge in `BuildDisplayRows` (rejected).

---

## D055 — `direct_peer_contact_id` catalog denorm

**Date:** 2026-06-29  
**Decision:** `profile.db` **`threads.direct_peer_contact_id`** stores the peer for **`kind=direct`** (= `ChatTargetKey.contact_id`). Index **`(kind, channel, direct_peer_contact_id)`**. **`FindOrCreateDirectThread`** uses **`chat_targets`** PK (D056); denorm supports catalog repair.  
**Rationale:** Fast catalog lookup; canonical key remains `ChatTargetKey`.  
**Alternatives:** JSON1 expression index only (deferred).

---

## D056 — Local `thread_id`; wire routes via `ChatTargetKey`

**Date:** 2026-06-29  
**Decision:** **`thread_id`** (stored as **`chat_targets.local_thread_id`**) is **local only** — never sent on relay envelope or included in E2E AAD. **Direct P2P wire routing:** `ChatTargetKey { contact_id: envelope.sender_contact_id, channel: envelope.route.channel }` → `FindOrCreateDirectThread` → persist to that device's `local_thread_id`. Envelope includes **`route`**: `{ "kind": "direct", "channel": "…" }` (**`[post-v1]`** group: `{ "kind": "group", "group_id": "…" }`). **Reject** envelopes containing `thread_id` (D016). **Single** wire + AAD layout — no legacy dual-parser. Relay backfill: **`GET /v1/chat-targets/messages?peer_contact_id=&channel=`** (D027).  
**Rationale:** Each device owns storage layout; logical conversation is `ChatTargetKey`; group-ready `route` object; one clean protocol cut with D016 wipe.  
**Alternatives:** Shared wire `thread_id` (D053 — superseded); flat `channel` field without `route` (rejected — poor group extensibility).

---

## Open decisions (not yet resolved)

| ID | Question | Options |
|----|----------|---------|
| — | *(none in this project — all O001–O005 resolved D023–D027)* | |

**Cross-project (e2e-message-crypto):** PSK entry UX (E-O003), automated key agreement (E-O004), group E2E (E-O005).  
**Cross-project (platform-safety-limits):** LLM response caps, profile JSON store limits — not chat wire scope.

When resolved, move rows to numbered decisions above.
