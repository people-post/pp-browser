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

## D002 — Thread index + per-thread directory (superseded on-disk detail by D025)

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — per-thread directory layout (D025).  
**Decision:** Retain `threads/index.json` for sidebar metadata. Each thread owns a **directory** `{thread_id}/` with separate files for messages, memory, and sync state — not a single flat `{thread_id}.json`.  
**Rationale:** Sidecar memory, atomic writes per artifact, room for attachments later; index stays small for fast sidebar load.  
**Alternatives:** Single file per thread (original D002); monolithic database.

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

## D009 — Three sync modes (tail, gap repair, scroll backfill)

**Date:** 2026-06-27  
**Decision:** P2P history sync uses three distinct modes: **(1) tail sync** — on open, reconnect, or new device, fetch latest N (default 50) messages per peer; **(2) gap repair** — automatic backfill when a hole appears in the contiguous tail (have seq N, receive N+2+), not gated on scroll; **(3) history backfill** — when user scrolls to the top of loaded transcript, page older messages (25 per page) via `sender_seq < loaded_min_seq`. Bootstrap/tail ingest on an empty store does **not** treat a high incoming seq as a gap.  
**Rationale:** Lazy history saves bandwidth; live gap repair keeps private chat trustworthy; conflating the two causes false alarms or missed holes.  
**Alternatives:** Full history sync on every open; poll cursor only (no per-thread gap detection).

---

## D010 — Seq lifecycle on chat target

**Date:** 2026-06-27  
**Decision:** `next_outgoing_seq` and `session_epoch` are keyed to **chat target `(contact_id, channel)`**, surviving thread delete/recreate and **clear visible history** (seq is not reset on clear). On clear, receiver sets `history_floor_seq[peer][epoch]` to the max contiguous seq at clear time; relay-visible seq at or below the floor in the same epoch is **compromised** (D013), not silently ignored. Failed sends retry with the **same `message_id` and `sender_seq`**. New device / full reset requires **epoch bump** (D014). `uint64` overflow is accepted as out of scope.  
**Rationale:** Seq represents the long-lived conversation with a contact, not a local transcript snapshot; idempotent retries must not bump seq; clear history is a local display choice, not a protocol reset; floor violations catch replay and sender reset without epoch bump.  
**Alternatives:** Reset seq on clear; assign seq only after successful relay; thread-scoped counters; silently ignore below-floor messages.

---

## D011 — Session compromise on conflicting seq

**Date:** 2026-06-27  
**Decision:** If the same `(sender, session_epoch, sender_seq)` is received with a **different `message_id`**, treat as **session integrity failure** (one case of D013): halt ingest, notify both parties, rotate E2E keys, bump `session_epoch`, and start a new secure conversation (seq resets **only** for the new epoch). Same `(message_id, sender_seq)` duplicates are benign (UUID dedup). Envelope signature must bind `sender_seq` and `session_epoch`.  
**Rationale:** Under encryption, conflicting seq implies replay, split-brain, or attack; silent merge would break trust in private chat.  
**Alternatives:** Last-write-wins; ignore conflict; log only.

---

## D012 — Three `@ai` modes in direct threads

**Date:** 2026-06-27  
**Decision:** Direct-thread `@ai` has three explicit modes: **(1) Local** — `@ai …`, private assist, not relayed, no sync seq; **(2) Shared reply** — `@ai+ …` (alias `@ai share …`), relay AI output only, +1 sync seq on trigger user’s stream; **(3) Shared full** — `@ai++ …` (alias `@ai share all …`), relay stripped prompt then AI reply, +2 sync seq. Default is local. Shared rows use **`generation=ai_on_behalf`** for the AI reply; wire identity and **`sender_seq`** belong to the **trigger user** (`seq_owner_contact_id=local:self`, envelope `sender_contact_id=local:self`) so peer gap detection stays on the user’s stream. Local `@ai` must not consume sync seq (avoids false gaps when private assists sit between relayed messages).  
**Rationale:** Users need private in-thread AI help without exposing it; when they choose to share, AI “speaks on behalf of” the triggerer with contiguous seq on the wire; prompt-only vs prompt+reply are distinct privacy/product choices.  
**Alternatives:** Single `@ai` always local; always relay AI replies; one seq stream including local assists (rejected — false peer gaps).

---

## D013 — Strict normal-or-compromised ingest (direct chat)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — scope extended to all direct threads (D018).  
**Decision:** In **all direct threads** (`public_relay` and `e2e`), the receiver accepts only messages matching a defined set of **normal** cases: benign duplicate, epoch advance, contiguous tail (`sender_seq == contiguous + 1` and above floor), tail bootstrap on empty per-epoch store, and authorized history backfill in `(floor, loaded_min)`. **`sender_seq ≤ history_floor_seq[peer][epoch]` in the same epoch is compromised** (replay, stale traffic, or sender violated within-epoch contract) — not silently ignored. **`sender_seq < contiguous_peer_seq`** without benign duplicate, **epoch decrease**, invalid signature, and **gap repair failure** are also compromised. **`sender_seq > contiguous + 1`** above floor is **gap** (repair allowed); repair exhaustion → compromised. Sync watermarks are keyed by **`(peer, session_epoch)`**. **E2E** compromised → PSK rotation + epoch bump; **public_relay** → integrity UX without PSK (delete thread / support).  
**Rationale:** Private and public direct chat both need fail-closed integrity; only recovery UX differs.  
**Alternatives:** E2E-only strict ingest; silently ignore below-floor messages; treat all gaps as compromised without repair; global seq watermarks across epochs.

---

## D014 — Peer reset requires `session_epoch` bump

**Date:** 2026-06-29  
**Decision:** Full peer reset (new device without backup, wiped chat-target sidecar, explicit “start over”) **must bump `session_epoch`** and reset `next_outgoing_seq = 1` for the new epoch only. The receiver treats a higher unseen epoch as a **fresh stream** (`sender_seq = 1` is normal bootstrap). Optional first relay-visible row: `kind=system`, `control_type=epoch_start` (consumes seq 1). **Sending `sender_seq = 1` without epoch bump in an established epoch** (where `contiguous_peer_seq > 0`) **is compromised**. Restored backup with same chat-target sidecar continues the existing epoch — not a reset.  
**Rationale:** Seq restart must be explicit and scoped; epoch is the namespace boundary; avoids ambiguous “fresh” traffic in an old epoch.  
**Alternatives:** Ad-hoc first-message flag without epoch; allow seq rewind on reinstall; reset seq on clear history.

---

## D015 — Single active sender per identity (v1)

**Date:** 2026-06-29  
**Decision:** v1 assumes **one active sending client per profile identity** per chat target. Running the same identity on two devices without coordination is unsupported — conflicting `sender_seq` triggers compromised ingest (D011). Document in settings/help; no device-scoped sub-seq or relay seq lease in v1.  
**Rationale:** Per-chat-target seq is simple and sufficient pre-launch; multi-device coordination is a large protocol surface.  
**Alternatives:** Device-scoped seq in envelope; central seq lease via relay; per-device PSK.

---

## D016 — No legacy thread migration

**Date:** 2026-06-29  
**Decision:** **No in-place upgrade** from pre-v2b/v6 on-disk thread JSON. Schema version bumps may require deleting `{data_dir}/profiles/{id}/threads/` (and chat-target sidecar when added). Acceptable because there are no production users yet.  
**Rationale:** Avoid migration code while the model is still evolving; devs wipe local data on breaking bumps.  
**Alternatives:** Backfill `sender_seq` on old messages; dual-read old/new schemas indefinitely.

---

## D017 — Durable outbox from thread store

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `registry.db` for dedup + outbox index (D028).  
**Decision:** Pending/failed `relay_visible` messages are the **durable outbox** — persisted in `thread.db` `messages` table before send. **`registry.db` `outbox` table** indexes pending/failed rows for O(1) startup scan (D028). In-memory retry queue is a performance layer only. Retries reuse same `(message_id, sender_seq)`. Relay idempotent on `message_id`.  
**Rationale:** Restart must not drop unsent messages; registry avoids opening every `thread.db` on startup.  
**Alternatives:** Full scan all thread DBs on startup; separate outbox file only.

---

## D018 — Strict ingest on public and E2E direct threads

**Date:** 2026-06-29  
**Decision:** D013 ingest classifier applies to **`public_relay` and `e2e` direct threads**. Recovery differs: E2E uses PSK rotation + epoch bump; public uses integrity banner and thread reset without crypto rotation.  
**Rationale:** Gap detection and seq integrity matter for all person-to-person chat, not only encrypted bodies.  
**Alternatives:** Relaxed ingest on public channel (timestamp + UUID only).

---

## D019 — Transcript display ordering

**Date:** 2026-06-29  
**Decision:** UI sorts `relay_visible` messages by `(session_epoch, sender_contact_id, sender_seq)`; local-only rows by `timestamp` among themselves; tie-break `timestamp` then `message_id`.  
**Rationale:** Sync integrity uses seq; timestamps may skew across devices; consistent scroll behavior during gap repair.  
**Alternatives:** Timestamp-primary sort; insertion order only.

---

## D020 — Reorder window before gap declaration

**Date:** 2026-06-29  
**Decision:** Hold out-of-order inbound messages in a reorder buffer of **`kReorderWindow = 32`** seq slots above `contiguous_peer_seq` before setting `sync_state=gap`.  
**Rationale:** Benign reorder during multi-path delivery or parallel repair should not alarm users.  
**Alternatives:** Zero buffer (immediate gap); larger window (more memory).

---

## D021 — `sender_contact_id` required on relay envelope

**Date:** 2026-06-29  
**Decision:** Relay envelope includes explicit **`sender_contact_id`** (contact id, not relay id). Ingest must not infer sender from `thread.participant_contact_ids[0]`.  
**Rationale:** Required for E2E AAD, group chat later, and correct per-sender seq streams.  
**Alternatives:** Infer from thread metadata; use `sender_relay_id` only.

---

## D022 — Receive pipeline step order

**Date:** 2026-06-29  
**Decision:** Ingest order is fixed: UUID dedup → signature verify → thread/epoch check → AEAD decrypt (e2e) → D013 classifier (with reorder buffer) → persist. See DESIGN.md § Receive pipeline. **Superseded in detail by D033** (envelope size before parse, plaintext size after decrypt).  
**Rationale:** Do not persist or advance watermarks before cryptographic and structural validation.  
**Alternatives:** Decrypt before signature; classify before decrypt.

---

## D023 — Sidebar grouped by channel category

**Date:** 2026-06-29  
**Decision:** Sidebar lists conversations in **named groups**: **AI** (`kind=ai`), **Public** (direct + `channel=public_relay`), **Private** (direct + `channel=e2e`). Same contact may appear once per group when both channels exist. Collapsible section headers; empty groups hidden or show placeholder.  
**Rationale:** Channel split (D004) needs visible separation; grouped lists scale better than interleaved rows.  
**Alternatives:** Two rows per contact without grouping; single list with badges only; nested contact → channels.

---

## D024 — Clear history as multi-level choice sheet

**Date:** 2026-06-29  
**Decision:** **Clear history** opens a **choice sheet** (not a single confirm). User picks one level per action:

| Level | Label (illustrative) | Transcript | AI memory | Thread shell | P2P floor |
|-------|----------------------|------------|-----------|--------------|-----------|
| `clear_visible` | Clear messages | wipe | keep | keep | set `history_floor_seq` |
| `clear_visible_and_memory` | Clear messages & AI memory | wipe | wipe | keep | set floor |
| `delete_conversation` | Delete conversation | gone | gone | delete | n/a |

P2P levels include disclosure that peer/relay may retain copies. **Forget AI memory** alone remains a separate menu action (memory only, transcript unchanged).  
**Rationale:** One “clear” button with explicit levels avoids wrong defaults (O002) and maps to D003 semantics.  
**Alternatives:** Separate menu items only; single clear with checkbox; default wipe memory.

---

## D025 — Per-thread directory (superseded file layout by D028)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `thread.db` replaces JSON files (D028).  
**Decision:** On-disk layout uses **one directory per thread** under `threads/{thread_id}/` containing **`thread.db`** (messages, memory, sync_state tables). Profile-level **`threads/registry.db`** for global `message_id` dedup and durable outbox index (D028). `threads/index.json` holds sidebar metadata only. Delete thread = remove directory + registry rows + index entry. **No migration** from legacy flat JSON (D016).  
**Rationale:** Directory delete semantics preserved; SQLite gives append, seq-range queries, and row-level delivery updates.  
**Alternatives:** `messages.json` + `memory.json` sidecars (rejected — skip intermediate JSON stage).

---

## D026 — Unified `ChatPayload` message format

**Date:** 2026-06-29  
**Decision:** All chat items (text, annotations, contact cards, crypto transactions, system controls) share one **wire and disk payload** shape. `ThreadMessage` carries `content_type` + JSON `payload` (+ optional `text` snippet for preview/search). Annotations are **first-class messages** with the same envelope and row model — UI adds special rendering (badge, inline on target, card chrome). See DESIGN.md § ChatPayload.

**Sync seq:** `relay_visible` payloads use normal seq rules (D008). Local-only reactions may use `relay_visible=false` without seq. LLM context includes `content_type=text` and selected `system` rows only.

**Rationale:** One parser, one relay schema, extensible to contact cards and on-chain txs without new envelope versions per type.  
**Alternatives:** Separate `kind=annotation` only; embedded mutations on target rows; type-specific envelope versions.

---

## D027 — Relay thread messages API (seq backfill)

**Date:** 2026-06-29  
**Decision:** Relay exposes **per-thread, seq-scoped message fetch** for tail sync, gap repair, and history backfill. Proposed shape in DESIGN.md § Relay API (D027). Inbox poll may remain for push/hints; **authoritative backfill** uses the thread endpoint. Send remains idempotent on `message_id`.  
**Rationale:** Gap repair when peer is offline requires relay indexed by `(thread_id, sender_contact_id, session_epoch, sender_seq)`.  
**Alternatives:** Inbox-wide seq index only; peer-only repair forever.

---

## D028 — SQLite per thread + `registry.db` from v2a

**Date:** 2026-06-29  
**Decision:** Phase **v2a** ships **`SqliteThreadStore`** (not an intermediate JSON-per-dir stage). Layout:

- `threads/index.json` — sidebar metadata (kind, channel, title, preview, unread)
- `threads/registry.db` — global `message_ids` dedup table; `outbox` index (`thread_id`, `message_id`, `delivery`) for D017 startup scan
- `threads/{thread_id}/thread.db` — `messages`, `memory`, `sync_state` tables

Vendor SQLite in `pp_base` (not libp2p fork). `IThreadStore` is the only feature seam; `JsonThreadStore` deprecated after cutover. **No JSON message files** in target layout. Wipe legacy `threads/*.json` on upgrade (D016).  
**Rationale:** v6 seq sync, durable outbox, and `ChatPayload` append path need indexed storage; per-thread DB preserves delete/clear isolation; registry avoids scanning every `thread.db` for `HasMessageId` and pending sends.  
**Alternatives:** JSON dirs in v2a then SQLite at v6; single monolithic `threads.db`; JSON `memory.json` sidecar (merged into `thread.db`).

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
| `kMaxChatActionsPerMessage` | **32** | `chat_actions` array |
| `kMaxPollBatchMessages` | **100** | Per inbox poll or relay fetch response |
| `kMaxRetryQueueItems` | **500** | In-memory + registry outbox rescan cap |
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
**Decision:** UI loads **pages** of messages, not full thread history. `IThreadStore::GetMessagesPage(thread_id, before_timestamp \| before_rowid, limit)` default **100**; scroll-up requests older pages. `BuildDisplayRows` operates on the loaded window only. Full-thread `GetMessages` retained for agent context policies that already trim — migrate to page API where possible.  
**Rationale:** Avoid O(n) memory and RML build per frame on long threads.  
**Alternatives:** Virtualized list with full load; always load last 50 only with no scroll-up.

---

## D032 — Relay poll backoff and batch limits

**Date:** 2026-06-29  
**Decision:** Do **not** call `PollAndMerge` every UI frame. **Foreground:** poll at most once per **2 s** (configurable); **background:** pause or **30 s**. Reject inbox responses with more than **`kMaxPollBatchMessages`** (D029) without processing. Align HTTP relay contract (D027) with `kMaxRelayEnvelopeJsonBytes`.  
**Rationale:** Today `ChatController::Update` polls every tick; wastes IO and merges unbounded batches.  
**Alternatives:** Push-only from relay; 5 s MCP throttle only.

---

## D033 — Ingest pipeline: size check before parse

**Date:** 2026-06-29  
**Updated:** extends D022.  
**Decision:** Inbound order: **(0) envelope byte size** → UUID dedup → signature verify → thread/epoch → decrypt (e2e) → **plaintext size** → JSON parse `ChatPayload` with schema validate → D013 classifier → persist. Reject oversize before `nlohmann::parse` on untrusted input.  
**Rationale:** JSON bombs and huge ciphertext must fail closed without full parse.  
**Alternatives:** Parse then check (rejected).

---

## Open decisions (not yet resolved)

| ID | Question | Options |
|----|----------|---------|
| — | *(none in this project — all O001–O005 resolved D023–D027)* | |

**Cross-project (e2e-message-crypto):** PSK entry UX (E-O003), automated key agreement (E-O004), group E2E (E-O005).  
**Cross-project (platform-safety-limits):** LLM response caps, profile JSON store limits — not chat wire scope.

When resolved, move rows to numbered decisions above.
