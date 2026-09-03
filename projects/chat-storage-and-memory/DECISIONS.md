# Decisions log

Record significant choices here so future sessions (human or agent) do not re-litigate them. Format: **ID**, **date**, **decision**, **rationale**, **alternatives considered**.

**Normative wire (promoted):** [`docs/contracts/WIRE_SCHEMAS.md`](../../docs/contracts/WIRE_SCHEMAS.md). **Compatibility policy:** [`docs/contracts/COMPATIBILITY.md`](../../docs/contracts/COMPATIBILITY.md). Links below to `WIRE_SCHEMAS.md` resolve via a stub in this folder.

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

## D004 — Separate threads per communicating identity and channel

**Date:** 2026-06-27  
**Updated:** 2026-07-02 — identity-keyed threads (D079); three tiers — both direct tiers E2E (D089).  
**Decision:** Thread identity for direct chats is **`ChatTargetKey` = `(peer_identity_kind, peer_identity_value, channel)`**, not local `Contact.id` alone. Two thread records (and files) for the same **communicating identity** when both **`e2e_public`** and **`e2e`** exist (D089). Same local **Contact** with two messaging identities → two unrelated threads.  
**Rationale:** Different crypto, transport, privacy UI, and AI memory boundaries; avoids mode-switch and identity-mix bugs.  
**Alternatives:** One thread with per-message encryption flags; single thread with “mode switch” rewriting history; one thread per Contact regardless of identity (rejected — D079).

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
**Rationale:** [AGENT_CONVERSATION.md](../../docs/ui/AGENT_CONVERSATION.md) already describes conversation-first design; dual models caused persistence gaps for AI sessions.  
**Alternatives:** Persist `Conversation` separately and sync to threads; SQLite-only unified table without JSON migration path.

---

## D008 — Sender-assigned `sender_seq` alongside UUID

**Date:** 2026-06-27  
**Decision:** Peer-visible direct `content` messages carry a monotonic **`sender_seq` (uint64) per sender per chat target**, in addition to UUID `message_id`. Seq is **per-sender**, not one global thread counter — in 1:1 chat each participant has an independent stream. UUID remains the sole message identity for dedup and annotations; seq is metadata for ordering and gap detection.  
**Rationale:** UUID dedup prevents duplicates but cannot detect missing messages; gap detection is critical for private chat reliability. Per-sender seq scales to group chat later.  
**Alternatives:** Relay-assigned global log index; positional indices as IDs (rejected in D005); timestamp-only ordering.

---

## D009 — E2E sync modes (tail + gap + manual; scroll deferred)

**Date:** 2026-06-27  
**Updated:** 2026-06-30 — unified backfill (D058); user-initiated sync in v6 (D059); scroll backfill still deferred (D052).  
**Decision:** **E2E direct tiers** (`e2e`, `e2e_public`) P2P history sync uses **`FetchChatTargetMessages`** (D058): **(1) tail sync** — on open, reconnect, or new device, fetch latest N (default 50) messages per peer; **(2) gap repair** — automatic backfill when a hole appears in the contiguous tail (have seq N, receive N+2+), not gated on scroll; **(3) user-initiated sync** — thread menu / retry banner (D059). **Scroll-driven history backfill** is **deferred** (D052) — v1 scroll-up uses local `GetMessagesPage` only. Bootstrap/tail ingest on an empty store does **not** treat a high incoming seq as a gap. Both **`e2e`** and **`e2e_public`** use this machinery (D089).  
**Rationale:** Tail + gap covers live private-chat reliability; scroll backfill is lower pre-launch value.  
**Alternatives:** Three modes in v1 (original D009); poll cursor only.

---

## D010 — Seq lifecycle on chat target

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — below-floor ingest after clear is sync exclusion + silent discard (D037), not compromised.  
**Decision:** `next_outgoing_seq` and `session_epoch` are keyed to **`ChatTargetKey`** **`(peer_identity_kind, peer_identity_value, channel)`** (D047/D079), surviving thread delete/recreate and **clear visible history** (seq is not reset on clear). On clear, receiver sets `history_floor_seq[peer][epoch]` to **`loaded_max_seq`** (max peer `sender_seq` in the deleted transcript — includes gap-repaired rows, D037); relay-visible seq at or below the floor in the same epoch is **excluded from sync** — silent discard, not compromised. Failed sends retry with the **same `message_id` and `sender_seq`** until clear cancels them (D024). New device / full reset requires **epoch bump** (D014). `uint64` overflow is accepted as out of scope.  
**Rationale:** Seq represents the long-lived conversation with a contact, not a local transcript snapshot; idempotent retries must not bump seq; clear history is a local display choice, not a protocol reset; floor marks a one-way local cutoff without protocol reset.  
**Alternatives:** Reset seq on clear; assign seq only after successful relay; thread-scoped counters; below-floor → compromised (superseded by D037).

---

## D011 — Session compromise on conflicting seq

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — recovery UX per D038; no continue-anyway (D046).  
**Decision:** If the same `(sender, session_epoch, sender_seq)` is received with a **different `message_id`**, treat as **session integrity failure** (E2E only, D045): **pause ingest and outbound**, record an integrity incident, show a choice sheet (D038, D046). **Recommended** recovery: rotate E2E keys + bump `session_epoch`. User may **not** choose continue-anyway in v1 (D046). Same `(message_id, sender_seq)` duplicates are benign (UUID dedup). Envelope signature must bind `sender_seq` and `session_epoch`. **Late fill** after authoritative empty close (D067) is **not** a rewind.  
**Rationale:** Under encryption, conflicting seq implies replay, split-brain, or attack; silent merge by default would break trust on **private (`e2e`)**. Informed override via relaxed ingest applies to **`e2e_public` / group** when those tiers ship (D046).  
**Alternatives:** Last-write-wins without disclosure (rejected); ignore conflict silently (rejected); log only (rejected).

---

## D012 — `@ai` in direct threads (local only v1)

**Date:** 2026-06-27  
**Updated:** 2026-06-29 — shared modes deferred; see [PHASES.md](PHASES.md) § Deferred.  
**Decision:** **Local `@ai`** plus shared `@ai+` / `@ai++` — [DESIGN.md § `@ai` in direct threads](DESIGN.md#ai-in-direct-threads-d012).  
**Rationale:** Matches shipped behavior; shared modes deferred.  
**Alternatives:** Three modes in v1 (original D012).

---

## D013 — Strict ingest (private direct only)

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — three tiers (D089); strict scope is **`e2e` private direct** only; **`e2e_public`** uses relaxed ingest (D046).  
**Decision:** In **`e2e` (private direct) threads**, the receiver uses D013: normal, gap, soft compromised, hard reject. Below-floor → D037 silent discard. Soft failures → pause + choice sheet (D038, D046). **`e2e_public`** direct: same seq machinery as private but **relaxed policy defaults** (D046). Sync watermarks keyed by **`(peer, session_epoch)`** on both E2E direct tiers.  
**Rationale:** Strict seq integrity on private direct; public/group tiers use same machinery with relaxed soft-failure policy (D046) when shipped.  
**Alternatives:** Strict ingest on both channels (original D013/D018).

---

## D014 — Peer reset requires `session_epoch` bump

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — no `epoch_start` row; epoch bump transaction (D047).  
**Decision:** Full peer reset **must bump `session_epoch`** via epoch bump transaction. Reset `next_outgoing_seq = 1`. **No `epoch_start` system message.** Restored backup with same `profile.db` + crypto sessions continues existing epoch. **Innocent peer** adopts the higher epoch on **first successful ingest** (passive adopt — D085), not only when locally initiating a bump.  
**Rationale:** Seq restart must be explicit and scoped; epoch is the namespace boundary; avoids ambiguous “fresh” traffic in an old epoch.  
**Alternatives:** Ad-hoc first-message flag without epoch; allow seq rewind on reinstall; reset seq on clear history.

---

## D015 — Single active sender per identity (v1)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — recovery UX per D038.  
**Updated:** 2026-08-13 — linked *devices* allowed; still one *sender* ([M016](../multi-device-account/DECISIONS.md#m016--dogfood-one-active-sender-on-linked-devices)). Dual-writer remains D074.  
**Decision:** v1 assumes **one active sending client per profile identity** per **E2E** chat target (D045). Linked installs of the same Account ID may all **receive**; **two senders** without coordination is unsupported — conflicting `sender_seq` triggers a **soft integrity failure** (D011): pause + choice sheet (D038), not silent merge. Document in settings/help; no device-scoped sub-seq or relay seq lease until D074.  
**Rationale:** Per-chat-target seq is simple and sufficient pre-launch; multi-device coordination is a large protocol surface.  
**Alternatives:** Device-scoped seq in envelope; central seq lease via relay; per-device PSK.

---

## D016 — No legacy migration

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — production disk uses migrate (D069); dev wipe for legacy only.  
**Decision:** **No in-place upgrade** from pre-v2b/v6 on-disk thread JSON or **legacy relay wire** (envelopes with `thread_id`, old AAD with `thread_id`). **Dev/pre-user:** may delete `{data_dir}/profiles/{id}/threads/` on cutover. **Shippable SQLite (`user_version=1`+):** incremental migration (D069), not wipe. **Single parser** for wire + AAD — no dual-version support.  
**Rationale:** Avoid migration code for obsolete layouts; production users must not lose transcripts on disk version bump.  
**Alternatives:** Backfill `sender_seq` on old messages; accept legacy envelopes alongside new (rejected).

---

## D017 — Durable outbox from thread store

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — `profile.db` holds `threads` catalog + `outbox` (D035).  
**Decision:** Pending/failed `relay_visible` messages are the **durable outbox** — persisted in `thread.db` `messages` table before send. **`profile.db` `outbox` table** indexes pending/failed rows for O(1) startup scan (D028). In-memory retry queue is a performance layer only. Retries reuse same `(message_id, sender_seq)`. Relay idempotent on `message_id`. Message-id dedup is **per-thread** in `thread.db` (D034), not in `profile.db`. Sidebar catalog is separate (`threads` table, D035).  
**Rationale:** Restart must not drop unsent messages; `profile.db` avoids opening every `thread.db` on startup.  
**Alternatives:** Full scan all thread DBs on startup; separate outbox file only.

---

## D018 — Ingest scope by tier (supersedes “strict on public”)

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — three tiers (D089).  
**Decision:** **`e2e` (private direct):** full D013 + D038 (strict only, D046). **`e2e_public`:** seq + sync with relaxed ingest default (D046). **`group`:** relaxed ingest when shipped (E022). Reject unknown direct channels (D090).  
**Rationale:** Tier-appropriate integrity cost.  
**Alternatives:** Strict ingest on all E2E tiers (rejected for public/group — D089).

---

## D019 — Transcript display ordering

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — UI sort via `display_order` (D054); seq/timestamp roles split.  
**Decision:** **UI / pagination:** `display_order ASC` on every message (D054). **`BuildDisplayRows`** and **`GetMessagesPage`** use `display_order` only. **E2E direct sync** (`e2e`, `e2e_public`): `(session_epoch, sender_contact_id, sender_seq)` for ingest and `GetMessagesBySeqRange` — not UI sort. `timestamp` is metadata, not transcript sort.  
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
**Updated:** 2026-07-02 — `sender_contact_id` = communicating identity **value** (D079); inbound policy split (D080).  
**Decision:** Relay envelope includes **`sender_contact_id`** and **`route`** (`kind` + `channel` for direct). **No `thread_id`** on wire. **`sender_contact_id`** is the sender's **communicating identity value** (e.g. `relay:abc123`, D079/D082) — not local `Contact.id` (D079). Inbound direct routing: `ChatTargetKey { peer_identity_kind, peer_identity_value: envelope.sender_contact_id, channel }` → local `local_thread_id` when a row exists (D080). Do not infer sender from local thread metadata.  
**Rationale:** Local thread ids differ per device; shared routing key is the chat target. `route` extensible to `group_id` later.  
**Alternatives:** Shared wire `thread_id` (D053 — superseded); infer sender from participants[0] (rejected).

---

## D022 — Receive pipeline step order

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — D080 auto-create at persist for `e2e_public`; D033 size check; see DESIGN § Receive pipeline.  
**Decision:** Ingest order: envelope size → **reject `thread_id` if present** → signature verify → parse `route` → **resolve `ChatTargetKey`** (D062/D080 — private find-only; public may **`pending_auto_create`**) → per-thread UUID dedup (when shell exists) → participant check → decrypt → **plaintext size** (D033) → parse `ChatPayload` → history floor (D037) → D013 classifier → persist (auto-create shell at persist for `e2e_public` when needed). **Single linear step list** in [DESIGN § Receive pipeline](DESIGN.md#receive-pipeline) — authoritative.  
**Rationale:** Dedup requires `local_thread_id` when present; wire carries no thread id; private inbound find-only prevents unwanted shells; `e2e_public` auto-create only after verify + decrypt (D080).  
**Alternatives:** Dedup before routing using envelope `thread_id` (superseded); create-on-ingest before decrypt (rejected — D062); ephemeral preview without persist (rejected — D090).

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
**Updated:** 2026-06-30 — confirmation dialog (D057); outbox purge; catalog reset.  
**Decision:** **Clear messages** (optional *Also forget what AI learned* checkbox) · **Delete conversation**. Separate **Forget what AI learned** menu item. **Clear messages** requires a **second-step confirmation dialog** with a pre-clear inventory (D057) before `ClearMessages` runs. **Clear messages** also: deletes pending/failed `relay_visible` rows; purges `profile.db` `outbox` for the thread; sets `preview=''`, `unread_count=0`. P2P disclosure in confirmation copy.  
**Rationale:** Simpler UX; same D003 semantics; users must understand cancelled sends and gap-repaired rows; avoid silent outbox loss.  
**Alternatives:** Three-level sheet (original D024); clear without confirmation (rejected).

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
**Updated:** 2026-06-30 — minimal `text` payload in v2a-p2p wire (D063); v4 validator hardening.  
**Decision:** One wire/disk payload shape: binary **ChatPayload** inside E2E AEAD plaintext (D087/D090). **v2a-p2p** ships **`body.e2e.payload_b64`** on direct wire (D063). **v4** validator accepts **`text`** and **`system`**; reject unknown `content_type` on inbound relay. Rich types — [DESIGN.md § ChatPayload](DESIGN.md#chatpayload-unified-message-body--d026).  
**Rationale:** One parser path; wire lands with routing cutover; v4 deepens validation without second break.  
**Alternatives:** All types in v1 (original D026); flat `body.text` until v4 (rejected — D063).

---

## D027 — Relay chat-target messages API (seq backfill, E2E)

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — identity query params (D079).  
**Decision:** Relay exposes seq-scoped fetch by **`ChatTargetKey`**. HTTP: **`GET /v1/chat-targets/messages`** with query params matching **`ChatHistoryRequest`** field names in [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) (`requester_identity_kind`, `requester_identity_value`, `peer_identity_kind`, `peer_identity_value`, `channel`, `session_epoch`, optional `min_sender_seq`, `max_sender_seq`, `limit`, `order`). **Authorization:** caller must be a party to that chat target; else **403**. Relay indexes by recipient inbox + sender identity + channel, not client `thread_id`. Send idempotent on `message_id`. **Reject** POST bodies with `thread_id`.  
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
**Decision:** Enforce explicit caps on chat wire, storage, and relay traffic. Constants in **`src/base/messaging/MessagingLimits.h`** (`namespace pbr`, `inline constexpr`); reject at compose, send, and ingest.

| Limit | Value | Applies to |
|-------|-------|------------|
| `kMaxComposeTextBytes` | **64 KiB** | User composer `text` |
| `kMaxChatPayloadBytes` | **64 KiB** | Serialized binary `ChatPayload` inside E2E plaintext (D087) |
| `kMaxE2ePlaintextBytes` | **128 KiB** | AEAD plaintext binary (E010); checked before/after decrypt |
| `kMaxRelayEnvelopeJsonBytes` | **256 KiB** | Full signed POST body |
| `kMaxContentRmlBytes` | **256 KiB** | **Local-only** assistant RML on disk (not accepted from wire) |
| `kMaxUserPayloadBytes` | **64 KiB** | Persisted `user_payload` on AI thread rows (aligns with platform-safety-limits) |
| `kMaxChatActionsPerMessage` | **32** | `chat_actions` array |
| `kMaxPollBatchMessages` | **100** | Per inbox poll or relay fetch response |
| `kMaxRetryQueueItems` | **500** | In-memory + `profile.db` outbox rescan cap |
| `kMaxOpenThreadDbs` | **16** | LRU of open `thread.db` handles |
| `kMaxDisplayPageMessages` | **100** | Default UI transcript window |
| `kMaxEmptyClosedSeqs` | **128** | Singleton entries in `empty_closed_seqs[]` before coalesce to ranges (D071) |
| `kMaxRetiredPskEpochs` | **8** | Max retired `(epoch, master_psk)` entries in OOB bundle export and `retired_psks_json` on disk (D086/E020) |
| `kMaxPskBundleBytes` | **4 KiB** | Serialized `pp-browser-psk-bundle-v1` JSON for OOB paste (E020) |

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
**Decision:** Inbound order per D022/D033: envelope size → reject legacy `thread_id` → signature → `route` → resolve `ChatTargetKey` → dedup → participant check → decrypt (e2e) → **plaintext size** → parse `ChatPayload` → history floor (D037) → D013 → persist. **Single linear step list** in [DESIGN § Receive pipeline](DESIGN.md#receive-pipeline) — no nested channel-branch duplicates. Reject oversize before `nlohmann::parse` on untrusted input.  
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
2. For **visible rows only** (current sidebar viewport / page slice — not all threads): if `{thread_id}/thread.db` is missing → delete `threads` + `outbox` rows for that id; else run `SELECT text, timestamp FROM messages ORDER BY display_order DESC LIMIT 1` and update cached preview/`updated_at` if mismatched (D054).
3. `FindOrCreateDirectThread(ChatTargetKey, participant_contact_id)` looks up **`chat_targets`**; catalog via **`threads.peer_identity_*`** + `channel` (D055, D079).

**Profile open (once):** `readdir` `threads/*/` — directories with `thread.db` but no `threads` row → insert stub catalog row (repairs crash-after-DB-create).

**Delete thread:** single `profile.db` transaction — `DELETE FROM threads` + `DELETE FROM outbox WHERE thread_id=?` — then remove `{thread_id}/` directory.

**Clear messages:** keep `threads` row; set `preview=''`, `unread_count=0`; purge `outbox`; visible verify shows empty preview when transcript is wiped (D024).

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
**Updated:** 2026-06-30 — floor = `loaded_max_seq` (not contiguous-only); outbox purge on clear.  
**Decision:** After **clear visible history** (D024), receiver sets **`history_floor_seq[peer][epoch]`** to **`loaded_max_seq[peer][epoch]`** immediately before delete — the maximum peer `sender_seq` present in the transcript, **including gap-repaired rows** (not `contiguous_peer_seq` alone). Then **`DELETE FROM messages`** (transcript and per-thread dedup surface wiped — D034), purge **`profile.db` `outbox`** for the thread, and reset `loaded_min`/`loaded_max`/`contiguous_peer_seq`. For the same epoch, any inbound or relay-fetched message with **`sender_seq ≤ history_floor_seq`** is a **sync exclusion zone**, not an integrity failure: **silent discard** — do not persist, backfill, show, or bump unread; do **not** enter `sync_state=compromised`. Applies to **all paths**: poll, direct, tail sync, gap-repair responses. Only **`sender_seq > floor`** is eligible for normal D013 ingest. **No resurrection** of cleared seq via tail sync in the same epoch. Clients must clamp relay fetch when floor is set: use **`min_sender_seq = floor + 1`**. **No post-clear message-id tombstone** in `profile.db`. Full conversation restart uses **epoch bump** (D014), not clear.  
**Rationale:** Contiguous-only floor allowed tail sync to repopulate gap-repaired messages after clear; max-seen floor matches “clear what I saw”; outbox purge aligns with deleted pending rows.  
**Alternatives:** Floor = contiguous only (rejected — tail sync resurrection bug); below-floor → compromised (original D013 wording).

---

## D038 — E2E integrity recovery (no continue-anyway)

**Date:** 2026-06-29  
**Updated:** 2026-06-29 — superseded relaxed ingest (D046); E2E-only scope (D045).  
**Decision:** On E2E soft integrity failure: **pause ingest/outbound**, append incident (ring buffer D049), show choice sheet with disclosure. User picks **`rotate_psk`** (start new secure chat) or **`pause_only`**. **No `continue_anyway`**, no `ingest_policy=relaxed`, no `trust_degraded` in v1. While compromised: **outbox frozen**, gap/tail sync disabled (D068). **`rotate_psk`** choice sheet includes E018/D083 disclosure (saved history, relay ciphertext, new epoch). Persist in `sync_state.state_json`:

| Field | Values |
|-------|--------|
| `user_resolution` | `null` \| `rotate_psk` \| `pause_only` |
| `user_acknowledged_at` | unix ms |
| `integrity_incidents[]` | ring buffer, max **10** (D049) |

Hard failures: pause + **Pause only** until delete thread or key rotation.  
**Rationale:** Strict recovery avoids dual classifier and ambiguous outbound interop.  
**Alternatives:** Continue anyway with relaxed ingest (original D038). Full rules: [DESIGN.md § Relaxed ingest](DESIGN.md#relaxed-ingest--continue-anyway--public-direct-and-group-d046).

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
**Decision:** `ICompactionService` runs when a thread’s **text turn count** (user + assistant `content_type=text` rows) exceeds **`kCompactionTurnThreshold = 20`** since the last summary version. Constants in `src/base/messaging/MessagingLimits.h` (or shared with `ContextBudget`):

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
**Decision:** Add explicit P2P retry/repair caps in `src/base/messaging/MessagingLimits.h`:

| Constant | Value | Applies to |
|----------|-------|------------|
| `kMaxOutboxRetryAttempts` | **12** | Per `message_id` durable outbox resend attempts (exponential backoff) |
| `kMaxGapRepairRounds` | **5** | Consecutive repair cycles per `(peer, epoch)` gap before soft compromised (D038) |
| `kMaxGapRepairSeqSpan` | **500** | Max `max_sender_seq - min_sender_seq + 1` per single repair fetch |

After **`kMaxOutboxRetryAttempts`**: set `delivery=failed`, keep row, show persistent send-failure affordance; user may retry manually (resets attempt counter). After **`kMaxGapRepairRounds`** of **transport failures** (not authoritative empty success per D061): pause + integrity choice sheet (D038). If startup `ListPendingOutbox` returns more than **`kMaxRetryQueueItems`** (D029), process first 500 in **`outbox.updated_at ASC`** order and log warning — do not drop rows.  
**Rationale:** “Cap attempts” and “repair exhaustion” in DESIGN were qualitative; implementers need constants; D061 empty close does not consume repair rounds toward compromise.  
**Alternatives:** Unlimited retries; immediate compromised on first repair miss; empty fetch counts as failed round (rejected — D061).

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

## D045 — E2E direct `sender_seq`; strict ingest on private tier only

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — three tiers (D089); seq on both **`e2e`** and **`e2e_public`**; D013 strict on **`e2e`** only.  
**Decision:** **`sender_seq` / `session_epoch` on the wire apply to both E2E direct tiers** (`e2e`, `e2e_public`). **Strict D013** applies to **`e2e` (private)** only. **`e2e_public`** uses the same sync/backfill path (D058) but relaxed ingest on soft failures (D046). Reject `public_relay` (D090).  
**Rationale:** Seq integrity matters for all encrypted direct chat; strict pause-on-conflict is reserved for the security-first tier.  
**Alternatives:** Seq on private tier only (original D045 — superseded); plaintext public channel (rejected — D089).

---

## D046 — Relaxed ingest tier-default for public direct; strict-only in v1 private

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — three tiers (D089); relaxed ingest is **default for `e2e_public` and group**; **`e2e` private** keeps strict-only.  
**Decision:** On soft integrity failure in **`e2e` (private direct)**, user picks **Start new secure chat** or **Pause only** — no `continue_anyway`. **`e2e_public`** and **`group`** use **`ingest_policy=relaxed`** by default with **`continue_anyway`** / LWW rules per [DESIGN § Relaxed ingest](DESIGN.md#relaxed-ingest--continue-anyway--public-direct-and-group-d046) — ships **with** the public/group tier (auto-key c3+ for `e2e_public`), not as an optional add-on after private v6. Hard crypto failures (bad sig, decrypt fail) remain non-overridable on all tiers.  
**Rationale:** Security-first tier must not silently merge conflicting seq; UX-first tiers accept tradeoffs and auto-recover where possible.  
**Alternatives:** Global strict for all E2E (original D046 — rejected for public/group tiers).

---

## D047 — `chat_targets` table in `profile.db`

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — PK `(peer_identity_kind, peer_identity_value, channel)` (D079); PSK columns (D084/E008).  
**Decision:** **`chat_targets`** PK = **`ChatTargetKey`** **`(peer_identity_kind, peer_identity_value, channel)`**. Columns: **`local_thread_id`** (current on-disk shell; **not on wire**), optional **`participant_contact_id`** (local Contact.id for catalog), **`next_outgoing_seq`**, **`session_epoch`**, and for **`channel` ∈ { `e2e`, `e2e_public` }** — **`master_psk_b64`**, **`psk_fingerprint`**, **`psk_verified_at`** (E011 send gate on **`e2e`**; optional/deferred on **`e2e_public`** — D089), **`retired_psks_json`** (D084). Updated under same writer mutex as `outbox`. **Delete direct conversation** removes shell but **keeps** `chat_targets` (seq/epoch/PSK). Shell recreate may allocate **new** `local_thread_id`.  
**Rationale:** Seq/epoch/PSK are per logical chat target (communicating identity + channel); local storage ids are device-private; single-row txn on epoch bump.  
**Alternatives:** Wire-stable `thread_id` (D053 — superseded); `sessions.json` sidecar (rejected — D084).

---

## D048 — No encryption at rest for thread DBs (v1) — **superseded by D102**

**Date:** 2026-06-29  
**Updated:** 2026-08-19 — **superseded by [D102](#d102--transcript-body-aead-under-profile-dek-no-migration)**.  
**Decision:** *(Historical.)* `thread.db` was plaintext SQLite.  
**Rationale:** Explicit assumption for transcripts; PSK/identity at-rest tracked separately (e2e E008 / at-rest A002).  
**Alternatives:** Encrypt all thread DBs in v2a.

---

## D102 — Transcript body AEAD under profile DEK (no migration)

**Date:** 2026-08-19  
**Supersedes:** [D048](#d048--no-encryption-at-rest-for-thread-dbs-v1--superseded-by-d102).  
**Decision:** Message bodies, sidebar previews, and thread memory summaries are **AEAD-encrypted under the profile DEK** (same PIN vault as `identity.enc` / PSK columns). One blob per row:

| Row | Column | AAD purpose |
|-----|--------|-------------|
| Message content pack | `messages.content_enc` | `transcript-body\|{profile_id}\|{thread_id}\|{message_id}\|1` |
| Sidebar preview | `threads.preview_enc` | `transcript-preview\|{profile_id}\|{thread_id}\|1` |
| Thread memory | `memory.value_enc` | `transcript-memory\|{profile_id}\|{thread_id}\|{key}\|1` |

Inner plaintext is a versioned **`TranscriptBodyCodec` v1** envelope (chat payload + denormalized presentation fields). **Metadata stays plaintext** (timestamps, delivery, `content_type`, `target_message_id`, thread titles, participant ids, `sync_state`). **No SQLCipher.** **No incremental migration** — bump `user_version` (`thread.db` **2**, `profile.db` **4**); incompatible DBs fail with wipe-profile guidance.  
**Implementation:** `SqliteThreadStore` implements `IDekConsumer`; registered from `ConversationsHub`. Chat history requires profile unlock when a vault exists.  
**Deferred:** FTS/search sidecar; encrypting thread titles / participant lists.  
**Rationale:** Protect offline disk reads of message text without full-DB encryption; reuse existing DEK/`FileCipher` stack.  
**Alternatives:** SQLCipher whole DB; per-column AEAD with online migration (rejected — hard cut).

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
**Updated:** 2026-06-30 — user-initiated sync in v6 via D059 (same D058 primitive); scroll trigger remains post-v1.  
**Decision:** **No fetch when user scrolls to top** in v1. Scroll-up uses **`GetMessagesPage`** on local transcript only. E2E v6 sync: **tail + gap repair + user-initiated sync** (D009, D059) via **`FetchChatTargetMessages`** (D058). Scroll-triggered history backfill: [DESIGN.md § P2P sync](DESIGN.md#p2p-sync-e2e-only--d045).  
**Rationale:** Scroll UX is extra surface; manual sync covers explicit “get history from peer” in v6.  
**Alternatives:** Three sync modes in v1 including scroll (original D009).

---

## D053 — Stable `thread_id` per direct chat target (superseded by D056)

**Date:** 2026-06-29  
**Superseded:** 2026-06-29 — D056 (local `thread_id`; wire uses `ChatTargetKey`).  
**Decision:** ~~Wire-shared stable `thread_id`~~ — do not implement.  
**Rationale:** Superseded — peers keep private local ids; routing via `sender_contact_id` + `route.channel`.

---

## D054 — `display_order` for UI sort and pagination

**Date:** 2026-06-29  
**Updated:** 2026-06-30 — UI defer rules (D065); scroll anchor `message_id` (D057).  
**Decision:** Every `messages` row has **`display_order INTEGER NOT NULL`**, assigned in **`AppendMessage`** per DESIGN § Display order assignment. **UI sort** and **`GetMessagesPage(before_display_order)`** use this column only. **`sender_seq`** remains for E2E sync/ingest only (D045). Default append: `max+1`; gap repair: insert between seq neighbors (renumber tail in one batch if needed). **Scroll anchor:** `message_id` only — not array index (D057). **Gap repair UI:** defer refresh per D065. **Sidebar preview verify** (D035): `ORDER BY display_order DESC LIMIT 1`.  
**Rationale:** Unifies AI, public, E2E, and local `@ai` transcript ordering; avoids channel-specific pagination cursors; preview matches transcript sort.  
**Alternatives:** `before_timestamp` pagination; runtime merge in `BuildDisplayRows` (rejected).

---

## D055 — Peer identity catalog denorm (superseded field name by D079)

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — D079: `peer_identity_kind` + `peer_identity_value` replace `direct_peer_contact_id`.  
**Decision:** `profile.db` **`threads`** stores **`peer_identity_kind`** + **`peer_identity_value`** for **`kind=direct`** (= `ChatTargetKey` identity fields). Index **`(kind, channel, peer_identity_kind, peer_identity_value)`**. **`participant_contact_ids`** holds local **Contact.id**. **`FindOrCreateDirectThread`** uses **`chat_targets`** PK (D056/D079); denorm supports catalog repair.  
**Rationale:** Fast catalog lookup; canonical key remains `ChatTargetKey`.  
**Alternatives:** JSON1 expression index only (deferred).

---

## D056 — Local `thread_id`; wire routes via `ChatTargetKey`

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — identity-keyed `ChatTargetKey` (D079); inbound channel split (D080).  
**Decision:** **`thread_id`** (stored as **`chat_targets.local_thread_id`**) is **local only** — never sent on relay envelope or included in E2E AAD. **Direct P2P wire routing:** resolve **`ChatTargetKey`** from **`envelope.sender_contact_id`** (communicating identity **value**, D079) + **`envelope.route.channel`** + inferred **`peer_identity_kind`** (v1: `relay_user` for relay path). **Outbound:** `FindOrCreateDirectThread` → persist to `local_thread_id`. **Inbound:** see D080 (`e2e` find-only; `e2e_public` auto-create after decrypt). Envelope includes **`route`**: `{ "kind": "direct", "channel": "…" }` (`group`: `{ "kind": "group", "group_id": "…" }`). **Reject** envelopes containing `thread_id` (D016). **Single** wire + AAD layout — no legacy dual-parser. Relay backfill: **`GET /v1/chat-targets/messages`** per D027 / [WIRE_SCHEMAS § ChatHistoryRequest](WIRE_SCHEMAS.md#chathistoryrequest-shared--relay-get--libp2p-d060).  
**Rationale:** Each device owns storage layout; logical conversation is `ChatTargetKey`; group-ready `route` object; one clean protocol cut with D016 wipe.  
**Alternatives:** Shared wire `thread_id` (D053 — superseded); flat `channel` field without `route` (rejected — poor group extensibility).

---

## D057 — v2a implementation guardrails + clear confirmation

**Date:** 2026-06-30  
**Updated:** 2026-06-30 — wire cutover (D063), memory retained copy (D064), type gates (D066).  
**Decision:** Before v2a merge, adopt these implementation rules:

| Area | Rule |
|------|------|
| **Phase split** | **v2a-core** (SQLite + AI threads + `GetMessagesPage` + clear UX) then **v2a-p2p** (`chat_targets`, outbox, per-thread dedup, E2E envelope shape — D063/D090) — see [PHASES.md](PHASES.md) |
| **Dual-DB writes** | Lock `profile.db` then `thread.db`; write `thread.db` txn first inside critical section — [DESIGN § SQLite operations](DESIGN.md#sqlite-operations-d044) |
| **`IThreadStore` cutover** | Extend API in one pass; grep gate: no `GetMessages` / profile-global `HasMessageId` in `src/feature/` |
| **`ThreadMessage` (v2a-core)** | Add **`display_order`** to C++ type; wire through store + UI paging (D066) |
| **`RelayEnvelope` (v2a-p2p)** | Final shape D056 + minimal ChatPayload body (D063); grep gate: no `envelope.thread_id` in `src/feature/` or `tests/` |
| **`ThreadContextPolicy`** | v3: filter `content_type=text` (+ selected `system`); compaction counts text turns only (D039, D040) |
| **Clear confirmation** | Two-step UX: choice sheet → **inventory confirmation dialog** (D024); **AI memory retained** section when forget unchecked (D064) |
| **Gap repair UI** | Defer display refresh per D065 when renumber affects loaded window |

**Rationale:** Review findings before implementation; reduce v2a blast radius; prevent tail-sync resurrection and silent outbox loss.  
**Alternatives:** Monolithic v2a PR (rejected); clear without detailed confirmation (rejected).

---

## D058 — Unified E2E backfill (`FetchChatTargetMessages`)

**Date:** 2026-06-30  
**Decision:** All **E2E** seq-scoped history fetch — tail sync, gap repair, user-initiated sync (D059), and scroll history backfill (D052) — share one feature-layer primitive **`FetchChatTargetMessages`**. Parameters: `ChatTargetKey`, `session_epoch`, optional `min_sender_seq` / `max_sender_seq`, `limit`, `order`. **Transport order:** **(1) libp2p peer-direct** (D060) when connected → **(2) relay** `GET /v1/chat-targets/messages` (D027). Ingest responses through the normal receive pipeline (D013, D037 floor). Respect `kMaxGapRepairSeqSpan` and `kMaxPollBatchMessages` (D029/D041).  
**Rationale:** One code path for “get missing peer messages”; direct preferred when both peers are online; relay remains fallback and for offline peers.  
**Alternatives:** Separate relay-only and direct-only implementations per sync mode (rejected).

---

## D059 — User-initiated sync from peer (v6)

**Date:** 2026-06-30  
**Decision:** E2E direct threads expose **user-initiated sync** in v6 (thread menu **Sync with peer**; gap banner **Retry sync**). Invokes **`FetchChatTargetMessages`** (D058): tail refresh + repair known gaps + optional older range `(history_floor_seq, loaded_min_seq)` when `loaded_min_seq > floor + 1`. **Failed outbound** rows (`delivery=pending`/`failed`) are **not** fixed by peer sync — user **retries send** or clears (D017/D024), **except while compromised** (D068). Copy must distinguish “sync missing messages from peer” vs “retry unsent message.” v6 ships with **relay fallback** when direct is unavailable; peer-direct (D060) preferred when libp2p is up. Scroll-to-top fetch uses the same primitive (D052).  
**Rationale:** Users expect direct P2P to resolve receive-side holes and older history; local-first outbox covers send-side failures separately.  
**Alternatives:** Scroll-only manual backfill (rejected); block composer until send succeeds (rejected).

---

## D060 — Peer-direct history protocol (libp2p)

**Date:** 2026-06-30  
**Decision:** libp2p app protocol **`/pp-browser/chat-history/1.0.0`** mirrors D027 semantics. **Request:** signed JSON — `requester_identity_kind`, `requester_identity_value`, `peer_identity_kind`, `peer_identity_value`, `channel`, `session_epoch`, optional `min_sender_seq` / `max_sender_seq`, `limit` (default 50, max 100), `order` (`asc`|`desc`). **Response:** `{ messages: RelayEnvelope[], has_more, cursor }` — same envelope shape as relay (no `thread_id`, D056). **Responder:** participant of `ChatTargetKey`; serve from local `GetMessagesBySeqRange` on **`local_thread_id`**; cap batch (D029). **Requester:** verify each envelope signature; ingest via receive pipeline. Reject non-participant requests. Wire spec: [DESIGN § Peer-direct history fetch](DESIGN.md#peer-direct-history-fetch-d060).  
**Rationale:** Peer-first backfill without a relay hop when both sides are online; reuses wire + ingest paths.  
**Alternatives:** Custom binary sync format (rejected); relay-only backfill (rejected — contradicts peer-first goal).

---

## D061 — Authoritative empty gap close (never-published seq)

**Date:** 2026-06-30  
**Updated:** 2026-06-30 — D067 guard + late fill; amends empty-close preconditions.  
**Decision:** After **`FetchChatTargetMessages`** (D058) returns **200 / success with zero envelopes** for a requested **single-seq** or **contiguous gap range** `[min, max]` (authenticated peer or relay, party to chat target), treat missing seq as **never published** — **advance `contiguous_peer_seq`** across the empty range **without** soft compromised (D038) — **only when D067 guard passes** (no local `relay_visible` row with `sender_seq > max` for that peer/epoch). On close: append closed seq values to **`empty_closed_seqs[]`** in `sync_state.state_json`. **Does not apply** when: transport error (retry repair round), response contains **conflicting** `(sender_seq, message_id)` vs local (D011), `session_epoch` mismatch, or **D067 guard fails** (higher seq already held — keep `sync_state=gap`, wait for live delivery). Still exhaust **`kMaxGapRepairRounds`** only on **transport/5xx** failures, not authoritative empty success. Send may fail locally while peer never saw that seq (D017); tail-only empty close + **late fill** (D067) covers abandoned outbound without false rewind on retry.  
**Rationale:** Seq is assigned pre-network (D010); abandoned outbound must not compromise the peer; empty authoritative fetch distinguishes skip from attack; guard prevents closing a hole while a higher seq proves lower may still be in sender's outbox.  
**Alternatives:** Block new sends until prior succeeds (rejected); always compromised after N empty rounds (rejected).

---

## D062 — Inbound direct routing: private find-only; public auto-create at persist

**Date:** 2026-06-30  
**Updated:** 2026-07-02 — channel-specific policy (D080).  
**Decision:** **Inbound** relay/direct delivery: resolve **`ChatTargetKey` → existing `local_thread_id`** via `chat_targets` when persisting. **Do not** `FindOrCreateDirectThread` on ingest **except** **`e2e_public` auto-create at step 12** after successful decrypt (D080). **Outbound** user actions (Secure message, Message shell, first send) create shell + catalog. Participant check (D027) before persist side effects.

| Channel | No `chat_targets` row | Missing shell |
|---------|----------------------|---------------|
| **`e2e` (private)** | Hard reject (cannot decrypt without PSK anyway) | Hard reject |
| **`e2e_public`** | Auto-create after successful decrypt (D080) | Hard reject if row exists |

**Product:** Private first message requires recipient to open secure chat + keys locally. Public tier auto-creates shell on first inbound decrypt.  
**Rationale:** Prevents signed-but-unwanted traffic from allocating storage; creation stays user-initiated.  
**Alternatives:** Create-on-ingest then delete on failed participant check (rejected).

---

## D063 — Wire cutover in v2a-p2p (envelope final; E2E body)

**Date:** 2026-06-30  
**Updated:** 2026-07-02 — no plaintext direct wire (D090).  
**Decision:** **v2a-p2p** ships the **final relay envelope shape** (D056): `sender_contact_id`, `route`, **no `thread_id`**; reject legacy envelopes (D016). **Direct body** uses **`body.e2e.payload_b64`** only (D090) — not legacy `RelayMessageBody { text, content_rml }` or **`body.content_b64`**. **Phase v4** adds validator hardening (`system` type, unknown-type reject, D030, size caps) — **not a second wire break**. Single parser for send/receive; no dual-version support. Direct P2P send/receive on wire requires E2E crypto (c2) + seq fields (v6).  
**Rationale:** D016 forbids two relay breaking changes; routing cutover and payload shape land together once; v4 deepens validation on the same wire.  
**Alternatives:** Legacy flat body until v4 (rejected — two cutovers); keep `thread_id` through v4 (rejected — D056).

---

## D064 — Clear confirmation: disclose retained AI memory

**Date:** 2026-06-30  
**Decision:** When user clears messages **without** **Also forget what AI learned**, the confirmation dialog (D057) **must** include an **AI memory retained** section: durable **`memory` table / conversation summary** stays; the AI may still use compacted context from cleared turns in future replies; use **Forget what AI learned** or the checkbox to wipe memory. When checkbox **is** checked, §3 **AI memory** (delete) applies instead — do not show retained section.  
**Rationale:** D003 allows independent transcript vs memory clear; default keep-memory is surprising without explicit copy.  
**Alternatives:** Always wipe memory on clear (rejected — D003); silent retain (rejected).

---

## D065 — `display_order` renumber: UI refresh defer rules

**Date:** 2026-06-30  
**Decision:** E2E gap repair **Rule 2** (D054) renumber must follow **scroll-stability rules** before `ChatController` refreshes display rows:

| Case | Rule |
|------|------|
| **Repair inserts entirely above** the loaded `GetMessagesPage` window | Update watermarks + DB only; **no UI list refresh** (no scroll jump). |
| **Repair touches rows inside** the loaded window (insert between neighbors or tail renumber) | **Defer** `BuildDisplayRows` until scroll anchor **`message_id`** is re-resolved; then refresh once. |
| **User pinned to anchor message** | After refresh, scroll position must still show the same **`message_id`** — never array index. |
| **Batch renumber** | One contiguous tail renumber pass per repair batch (D054); compute affected `message_id` set before commit to decide defer vs skip refresh. |

**Rationale:** Integer `display_order` insert/renumber during gap repair must not jump the transcript while the user is reading.  
**Alternatives:** Always refresh immediately (rejected); disable gap repair renumber (rejected).

---

## D066 — C++ type migration gates (`ThreadMessage`, `RelayEnvelope`)

**Date:** 2026-06-30  
**Decision:** Extend **`ThreadTypes.h`** in step with store phases — do not lag SQLite schema.

| Phase | C++ / wire requirement |
|-------|------------------------|
| **v2a-core** | `ThreadMessage.display_order` (`int64_t`); **`chat_payload`** BLOB or equivalent store field (D078/D087); `GetMessagesPage` / `AppendMessage` assign and read it. Other v4/v6 columns may remain store-only until wired. |
| **v2a-p2p** | **`RelayEnvelope`:** remove `thread_id`; add **`envelope_version`**, `sender_contact_id`, `route`; **`body.e2e.payload_b64`** (D063/D090). **Grep gate:** reject `public_relay`, `body.content_b64`, `envelope.thread_id`, `body.text` in `src/feature/` (legacy rejection tests excepted). |
| **v4** | `ThreadMessage.content_type`, `payload`; full ChatPayload codec + validator. |
| **v6** | `sender_seq`, `session_epoch` on `ThreadMessage` + envelope. |

**Rationale:** Prevents half-migrated types during cutover; grep gates match D057 store gates.  
**Alternatives:** Big-bang type change at v4 (rejected — P2P routing needs envelope shape in v2a-p2p).

---

## D067 — Empty gap close guard + late fill (D061 amendment)

**Date:** 2026-06-30  
**Decision:** Amends D061 to avoid false compromise when durable outbox (D017) allows higher `sender_seq` on the wire before a lower seq is relayed.

**Guard — do not empty-close** gap range `[min, max]` when **any** local `relay_visible` row exists for that `(peer_identity_kind, peer_identity_value, session_epoch)` with `sender_seq > max`. Keep `sync_state=gap`; rely on live delivery of the missing seq or transport exhaustion (D041). Example: have peer seq **4** and **6**, gap at **5** — empty fetch for **5** must **not** close; sender may still retry **5** from outbox.

**Empty-close when guard passes:** tail gaps and holes with no higher seq held — advance `contiguous_peer_seq`; append each closed seq to **`empty_closed_seqs[]`** or coalesce into **`empty_closed_ranges[]`** (D071).

**Late fill:** Inbound at seq `S` where `S ∈ empty_closed_seqs[]`, no existing row at `(peer, epoch, S)`, and unseen `message_id` → **normal accept** (D013), not rewind compromise. Remove `S` from `empty_closed_seqs[]` on persist. Covers tail empty-close followed by sender retry (D017).

**Rationale:** D061 + monotonic pre-send seq assignment (D010) conflict without guard; late fill covers legitimate retry after tail-only empty close.  
**Alternatives:** Serialize relay-visible sends — block seq N+1 while N is pending (rejected — stalls UX); empty-close always (rejected — false rewind).

---

## D068 — Compromised thread: outbox freeze, sync pause, epoch pending cancel

**Date:** 2026-06-30  
**Decision:** While `sync_state=compromised` (including **`pause_only`** resolution):

- **No** background outbox retry and **no** manual **Retry send** (D017/D059 copy must not contradict).
- **No** auto gap repair, tail sync, or **Sync with peer** — integrity choice sheet only.
- **`rotate_psk` / epoch bump (D014):** before incrementing epoch, **cancel** all `relay_visible` `pending`/`failed` rows for the **old** `session_epoch` on that chat target; purge matching **`profile.db` `outbox`** rows (D068). User re-composes in the new epoch.

**Epoch bump coordinator:** single feature-layer flow holds **`profile.db` mutex** while updating `chat_targets` (seq, epoch, PSK — D084) and `outbox` — see DESIGN § Epoch bump transaction. On **`rotate_psk`**, append retired PSK to `retired_psks_json` before replacing active key (E018/D083).

**Rationale:** Prevents livelock (retry send while paused); stale-epoch envelopes must not auto-resend after rotation; avoids cross-store race on epoch bump.  
**Alternatives:** Allow manual retry while compromised (rejected — ambiguous trust state); auto-resend pending after epoch bump (rejected).

---

## D069 — Schema evolution: migrate in production, wipe dev-only

**Date:** 2026-06-29  
**Decision:** **`PRAGMA user_version`** on `thread.db` and `profile.db` uses **incremental forward migrations** for shippable layouts (`user_version=1`+). **D016 wipe** applies only to **legacy JSON**, pre-v1 SQLite, and **dev/pre-user** builds — not to production disk bumps. **Breaking wire cutover** (`envelope_version`, D016 relay wipe) is independent of disk migration. Implement `Migrate(from→to)` before public release.  
**Rationale:** Review finding — wipe-on-bump is acceptable pre-launch but becomes unacceptable maintenance debt once users exist.  
**Alternatives:** Permanent wipe on every schema bump (rejected for production); no `user_version` (rejected).

---

## D070 — `memory` table key namespace + `ConversationSummary` JSON schema

**Date:** 2026-06-29  
**Decision:** `thread.db` **`memory`** table uses namespaced keys: **`summary`** → `ConversationSummary` JSON (`schema_version`, `version`, `text`, optional `compacted_through_display_order`, `updated_at`); **`fact:{uuid}`** reserved **`[future]`**. Summary `schema_version` bumps independently of SQLite `user_version`.  
**Rationale:** KV table needs documented value shapes before v3 compaction; avoids ad-hoc JSON per implementer.  
**Alternatives:** Opaque JSON only (rejected); separate `summaries` table (deferred).

---

## D071 — Cap empty closed seq metadata in `sync_state`

**Date:** 2026-06-29  
**Decision:** Track authoritative empty gap closes (D061/D067) with **`empty_closed_seqs[]`** (singletons) and **`empty_closed_ranges[]`** (`{min,max}` inclusive). **`kMaxEmptyClosedSeqs = 128`** — before append, **coalesce consecutive** closed seqs into ranges. **Late fill** checks both structures.  
**Rationale:** Unbounded `empty_closed_seqs[]` grows with abandoned pre-send seq assignments (D010/D017).  
**Alternatives:** Unlimited array (rejected); normalized `closed_seq` SQL table (deferred).

---

## D072 — `envelope_version` + shared history wire types

**Date:** 2026-06-29  
**Decision:** Every **`RelayEnvelope`** includes **`envelope_version: 1`** in v2a-p2p+; value is included in **Ed25519 canonical signing bytes**. Canonical **byte layout**, body hash, and `EnvelopeSigner` API: [e2e-message-crypto E014](../e2e-message-crypto/DECISIONS.md#e014--canonical-ed25519-relay-envelope-signing-bytes) / [DESIGN § Ed25519 signing](../e2e-message-crypto/DESIGN.md#ed25519-canonical-signing-bytes). Bump independently of `ChatPayload.payload_version` (D087) and SQLite `user_version`. **`ChatHistoryRequest` / `ChatHistoryResponse`** are **one C++ struct pair** shared by relay `GET /v1/chat-targets/messages` (D027) and libp2p `/pp-browser/chat-history/1.0.0` (D060). Normative wire: [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md).  
**Rationale:** Outer wire evolution without dual-parser; prevent relay/libp2p request shape drift.  
**Alternatives:** Implicit optional fields only (rejected); separate HTTP vs libp2p structs (rejected).

---

## D073 — Unknown-field policy on wire JSON

**Date:** 2026-06-29  
**Decision:** **`RelayEnvelope`:** ignore unknown **top-level** keys after required fields parse. **`ChatPayload`:** ignore unknown keys inside **`payload`** for known `content_type`; **reject** unknown `content_type` on **relay ingest**. **`ChatHistoryRequest/Response`:** reject unknown top-level keys. Unknown envelope keys are **not** signed unless a future signing spec adds them via `envelope_version` bump.  
**Rationale:** Forward-compatible wire extensions without breaking older clients; strict API for negotiated history fetch.  
**Alternatives:** Reject all unknown keys everywhere (rejected — too brittle for envelope); ignore all unknown everywhere (rejected for history API).

---

## D074 — Multi-device extension point (`sender_instance_id`)

**Date:** 2026-06-29  
**Decision:** **`[future]`** optional envelope field **`sender_instance_id`** (UUID per client install). **Omit in v1**; v1 assumes single active sender (D015). When multi-device ships, instance id participates in signing canonical bytes under a new **`envelope_version`**. Do not repurpose `sender_relay_id`.  
**Rationale:** D015 is a product constraint, not a permanent protocol limit — reserve field now to avoid another D016 cutover.  
**Alternatives:** Device sub-seq in `sender_seq` (deferred); no reserved field (rejected).

---

## D075 — `{thread_id}/blobs/` attachment placeholder

**Date:** 2026-06-29  
**Decision:** Per-thread directory layout reserves **`{thread_id}/blobs/`** for **`[future]`** content-addressed attachments (`blobs/{hash}`). Empty/unused in v1. `ChatPayload` rich types reference blobs by hash when implemented.  
**Rationale:** Media/file payloads need layout reservation before post-v1 rich types ship.  
**Alternatives:** Profile-wide blob store (deferred); inline BLOB in SQLite (rejected for large media).

---

## D076 — Group chat placeholders in catalog + sync scope

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — group tier UX-first E2E with pairwise keys (D089, E022).  
**Decision:** `profile.db` **`threads.group_id`** column (nullable, unused v1) for **`kind=group`**. Wire uses `route.kind=group` + `group_id` (D056). **`sync_state`** v1 PK `(peer_identity_kind, peer_identity_value, session_epoch)` is the **1:1 specialization**; groups use `(scope_kind, scope_id, session_epoch)` with **`scope_id = group_id`**. Group crypto: **pairwise sender-keys**, not a single group PSK (E022). Ingest policy matches **`e2e_public`** (relaxed default — D046).  
**Rationale:** Avoid second catalog migration when groups arrive; pairwise keys align with UX-first tier without a weak shared group secret.  
**Alternatives:** Add `group_id` only when groups ship (acceptable but higher migration cost); single group PSK (rejected — E022); reuse `ChatTargetKey` for groups (rejected — D056).

---

## D077 — `display_order` gap renumber as v1 complexity budget

**Date:** 2026-06-29  
**Decision:** **v1** keeps D054 **Rule 2** integer insert/renumber + D065 UI defer rules as the display-order model. Document as **ongoing complexity budget**, not eternal architecture. **`[future]`** alternatives: lexicographic order keys (fractional indexing) or separate **`ui_order`** graph by `message_id` — not scheduled.  
**Rationale:** Rule 2 works for v1 but is high edge-case surface; recording escape hatches avoids painting into a corner.  
**Alternatives:** Switch to order keys in v2a (rejected — scope); disable gap-repair renumber (rejected — breaks transcript interleaving).

---

## D078 — Canonical on-disk body: `chat_payload` BLOB + single encoder

**Date:** 2026-06-29  
**Updated:** 2026-07-02 — binary `ChatPayload` (D087); supersedes `chat_payload_json`.  
**Decision:** `messages.chat_payload` stores the full **`ChatPayload`** binary (wire-aligned per [D087](#d087--binary-chatpayload-v1-e014-body_hash--e010-plaintext)). Columns `content_type`, `payload`, `text`, `control_type`, `target_message_id` are **denormalized caches** written **only** via **`ChatPayloadCodec::EncodeToRow`** — never updated independently on the hot path. Local-only: `content_rml`, `user_payload`, `chat_actions`.  
**Rationale:** One canonical body simplifies migrations and matches E2E/plaintext wire; denormalized columns keep indexed queries without parsing binary every read.  
**Alternatives:** Split columns only with no canonical blob (rejected — drift risk); blob-only row with no denorm (rejected — query/index cost); JSON on disk (rejected — D087).

---

## D079 — Local contact vs communicating identity; identity-keyed `ChatTargetKey`

**Date:** 2026-07-02  
**Supersedes in detail:** D055 (`direct_peer_contact_id` denorm), D056 (`ChatTargetKey.contact_id` semantics).  
**Decision:**

1. **`Contact.id`** (address book) is **local only** — never on wire, never in AAD, never in `ChatTargetKey`. A **Contact** represents a person or entity and may hold multiple **`ContactId`** entries (`relay_user`, `peer_id`, `blockchain`, `custom`) — see `ContactTypes.h`. **`blockchain`** values use **CAIP-10** ([D091](#d091--blockchain-contact-id-caip-10-e024)). Some identities are used for messaging; others are metadata only.
2. **`ChatTargetKey`** (direct P2P) = **`(peer_identity_kind, peer_identity_value, channel)`** — the **communicating identity** bound when the thread is created, plus channel. **Not** local `Contact.id`. Same human with two messaging identities (e.g. two `relay_user` ids, or relay vs libp2p `peer_id`) → **separate threads** and separate PSK/seq state. Identity is **fixed for the life of the thread** — do not switch mid-thread; open a new thread for a new identity.
3. **Wire `sender_contact_id`** (envelope + AAD + signing bytes) carries the sender's **communicating identity `value`** (e.g. `relay:abc123`, libp2p peer id string — see D082) — **not** local `Contact.id`, **not** `local:self`. **`local:self`** remains a **local transcript sentinel** only (`ThreadMessage.sender_contact_id` on outbound rows).
4. **Outbound identity** for a thread is implied by transport + thread binding — user does **not** pick among their identities per send within the same thread. Relay threads use the profile's primary **`relay_user`** identity; future libp2p-direct threads use the bound **`peer_id`**.
5. **`threads.participant_contact_ids`** stores the local **Contact.id** (UI grouping, contact card). **`chat_targets`** and catalog denorm store **`peer_identity_kind`** + **`peer_identity_value`**. **`[later]`** optional local merge: relate multiple identities to one Contact without merging threads.
6. **PSK + seq/epoch** on **`chat_targets`** PK `(peer_identity_kind, peer_identity_value, channel)` — see [e2e-message-crypto E008/D084](../e2e-message-crypto/DECISIONS.md#e008--psk-store-v1-in-profiledb-chat_targets-at-rest-encryption-deferred). **HKDF `info`** uses `channel` + `epoch` only (E015) — not identity strings; pair scoping is the per-target `master_psk`.

**Rationale:** Separates address-book identity from routable messaging endpoints; fixes AAD self-check ambiguity; matches existing `ContactId` vector model; supports identity rotation as new thread + epoch without conflating local contacts.  
**Alternatives:** Wire-stable id per Contact (rejected — conflates local book with routing); rename wire field to `sender_identity` in v1 (deferred — keep JSON key `sender_contact_id`, document semantics).

---

## D080 — Inbound routing: private find-only; public auto-create

**Date:** 2026-07-02  
**Updated:** 2026-07-02 — three tiers (D089); no `public_relay` (D090).  
**Amends:** D062 (channel-specific inbound policy).  
**Decision:**

| Channel | No matching `chat_targets` row | Behavior |
|---------|-------------------------------|----------|
| **`e2e` (private)** | yes | **Hard reject** before decrypt/persist (D062). User must open **Secure message** with that peer identity and install PSK first. |
| **`e2e_public`** | yes | **Auto-create** shell + keys after successful decrypt (D089). Participant gate: verified Ed25519 + **`PeerSigningKeyStore`** entry (D081); see [DESIGN § Inbound auto-create](DESIGN.md#inbound-auto-create-e2e_public-only--d080). |

**Private E2E** with matching row but missing PSK / decrypt failure: hard reject. **`e2e_public`** with matching row: normal persist. Reject `public_relay` (D090). Participant check (D027) applies when a row exists.

**Rationale:** Private tier requires explicit key setup; public tier must not drop first contact.  
**Alternatives:** D062 strict on all channels (rejected for public UX).

---

## D081 — Peer signing key lookup before envelope verify (E016)

**Date:** 2026-07-02  
**Updated:** 2026-07-02 — lookup via **`IPeerSigningKeyResolver`** (E024); relay is v1 backend.  
**Cross-project:** [e2e-message-crypto E016](../e2e-message-crypto/DECISIONS.md#e016--peer-signing-keys-relay-directory-source-local-cache-oob-fingerprint-at-add), [E024](../e2e-message-crypto/DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007).  
**Decision:** Receive pipeline **step 2** (D022) resolves **`signing_public_key_b64`** for **`envelope.sender_contact_id`** (+ inferred **`peer_identity_kind`**, v1: `relay_user`) via **`IPeerSigningKeyResolver`** → **`PeerSigningKeyStore`**. v1 backend: **`RelayDirectoryResolver`** — directory hits + lazy **`GET /v1/users/{relay_user_id}`**, persist with provenance, then **`EnvelopeSigner::Verify`**. **Fail closed** if key is missing or verify fails — do not decrypt (E2E) or parse untrusted body. Keys are populated at **add-contact** from directory hits (`signing_public_key_b64` field) and optional manual paste; OOB fingerprint display at add (E016). **PSK** and signing keys are independent. **Do not** encode signing material in `sender_relay_id` or `sender_contact_id`.  
**Rationale:** Wire `sender_contact_id` is a communicating identity (D079), not an Ed25519 key; c2 production ingest requires an explicit identity→key binding. Directory is the relay-side registry; lazy fetch on cache miss supports D080 first inbound auto-create without per-message directory calls.  
**Alternatives:** Directory fetch on every message (rejected — latency, offline); TOFU first message (rejected — weak trust); key embedded in relay id (rejected — E016).

---

## D082 — Relay-user communicating identity string format

**Date:** 2026-07-02  
**Cross-project:** [e2e-message-crypto E017](../e2e-message-crypto/DECISIONS.md#e017--relay-user-identity-value-format).  
**Decision:** For **`peer_identity_kind = relay_user`**, the communicating identity **value** is:

```
relay:<opaque_id>
```

where `<opaque_id>` is **relay-assigned** at registration, **URL-safe** (`[A-Za-z0-9_-]{4,64}`), **immutable** for the life of the account, and **not** derived from the Ed25519 public key.

| Field / store | v1 rule |
|---------------|---------|
| `peer_identity_value` / `ContactId.value` | `relay:<opaque_id>` |
| `sender_contact_id` (wire, AAD, sign bytes) | Same string |
| `sender_relay_id` (wire) | **Same string** in v1 |
| `identity.json` `relay_user_id` | Relay-assigned after `register_user`; never synthesized from pubkey on wire |
| Directory / lazy lookup | `GET /v1/users/{relay_user_id}` uses the full value including `relay:` prefix |

**Byte rules (crypto binding):** UTF-8, exact bytes, case-sensitive, no trimming or Unicode normalization.

**Rejected formats:**

- `relay:user:<id>` — redundant with `peer_identity_kind`; was draft doc nomenclature only.
- `relay:` + truncated `public_key_b64` — not reversible (E016); local bootstrap placeholder only until registration.
- Bare `<opaque_id>` without `relay:` prefix — breaks existing relay API field naming and directory samples.

**Test fixture id:** `relay:alice123` — used in E014 frozen vectors, WIRE_SCHEMAS examples, and mocks.

**Rationale:** One canonical string bound in AAD, signing, and directory lookup; kind enum carries transport/type; relay remains source of truth for assignment.  
**Alternatives:** URI-style `relay:user:` segment (rejected — longer on wire, no relay adoption); bare opaque id (rejected — API churn).

---

## D083 — Retired PSK ledger on `rotate_psk` (E018)

**Date:** 2026-07-02  
**Cross-project:** [e2e-message-crypto E018](../e2e-message-crypto/DECISIONS.md#e018--retired-psk-ledger-for-historical-decrypt-after-rotate_psk), [E008/D084](../e2e-message-crypto/DECISIONS.md#e008--psk-store-v1-in-profiledb-chat_targets-at-rest-encryption-deferred).  
**Decision:** Epoch bump coordinator **`rotate_psk`** path must append the previous `(session_epoch, master_psk_b64)` to **`chat_targets.retired_psks_json`** before replacing the active PSK and incrementing epoch (E018/D084). **Epoch-only** bump ([D014](#d014--peer-reset-requires-session_epoch-bump), same `master_psk`) does **not** append a retired entry. All PSK + seq updates in one **`profile.db` transaction**.

**Decrypt:** feature layer resolves `master_psk` by **`envelope.session_epoch`** (E019/D085) via `IPskSessionStore::ResolveMasterPskForEpoch` — active PSK or retired ledger — then HKDF (E015). Never use lagging `chat_targets.session_epoch` for inbound decrypt. Messages already in `thread.db` as plaintext (`chat_payload`, D048/D069) remain readable without decrypt.

**Pruning:** optional — drop retired entry for epoch `E` when no local transcript rows for `E`, user cleared/abandoned that epoch sync surface, and no pending old-epoch sync work (E018). Hard cap: **`kMaxRetiredPskEpochs` (8)** — prune lowest epochs when ledger exceeds cap (D086/E020).

**c3 UX (D038 choice sheet):** disclose that saved-on-device history stays readable; pre-rotation relay ciphertext remains decryptable on this device via retained retired PSKs; new traffic requires new PSK; other devices need new PSK for the new epoch.

**Rationale:** Single `master_psk_b64` per target breaks historical decrypt after PSK rotation; chat-storage DESIGN already assumes old-epoch decrypt for backfill. Ledger is minimal vs re-encrypt-at-rotation (local bodies are plaintext).  
**Alternatives:** Accept permanent loss of pre-rotation relay ciphertext (rejected — contradicts DESIGN § Integrity recovery); re-encrypt local history at rotation (rejected — wrong layer, heavy).

---

## D084 — PSK columns on `chat_targets` in `profile.db` (E008)

**Date:** 2026-07-02  
**Cross-project:** [e2e-message-crypto E008](../e2e-message-crypto/DECISIONS.md#e008--psk-store-v1-in-profiledb-chat_targets-at-rest-encryption-deferred).  
**Decision:** E2E PSK material is stored on **`profile.db` → `chat_targets`**, not in `profiles/{id}/crypto/sessions.json` or per-thread `thread.db`. Columns ( **`e2e` and `e2e_public` channels**):

| Column | Type | Notes |
|--------|------|-------|
| `master_psk_b64` | TEXT NULL | 32-byte key, RFC 4648 base64; `NULL` until user installs PSK |
| `psk_fingerprint` | TEXT NULL | BLAKE2b display (E011) |
| `psk_verified_at` | INTEGER NULL | Unix ms when user confirmed OOB fingerprint match (E011); `NULL` until verified; cleared on PSK replace/import/rotation |
| `retired_psks_json` | TEXT NULL | JSON array `[{ epoch, master_psk_b64, retired_at }]` — E018 |

**`IPskSessionStore`** (`base/crypto`) + **`SqlitePskSessionStore`** (`feature/conversations/`) read/write these columns under the **`profile.db` writer mutex**. Epoch bump (D068) updates PSK + `session_epoch` + `next_outgoing_seq` in the **same transaction** — no cross-file sync.

**Rationale:** Chat-target-scoped secrets colocated with seq/epoch; survives thread shell delete/recreate; inbound decrypt resolves `ChatTargetKey` before `local_thread_id`.  
**Alternatives:** `sessions.json` sidecar (rejected); PSK in `thread.db` (rejected — ephemeral shell).

---

## D085 — Passive epoch advance (peer bumps first)

**Date:** 2026-07-02  
**Cross-project:** [e2e-message-crypto E019](../e2e-message-crypto/DECISIONS.md#e019--decrypt-and-hkdf-use-envelope-session_epoch).  
**Decision:** When the **peer** bumps `session_epoch` first (device reset, **Start new secure chat**, or epoch-only restart), the **innocent** device adopts the new epoch on **first successful E2E ingest** — not via a separate background job. This mirrors the local epoch bump coordinator's durable effects on `chat_targets`, but is **ingest-triggered**.

### Decrypt / HKDF (inbound)

- **`MessageCipher::Decrypt`** and **`SessionKeyDeriver`** MUST use **`envelope.session_epoch`** for HKDF `info` (E015) and for **`IPskSessionStore::ResolveMasterPskForEpoch`** — **never** `chat_targets.session_epoch` (which may lag).
- Outbound encrypt/sign continues to use **`chat_targets.session_epoch`** after adopt; until adopt completes, outbound MUST NOT emit relay-visible E2E envelopes (mutex + `sync_state` / compromised gates already pause sends when appropriate).

### Classifier rule 2 (D013)

**Epoch advance** — `envelope.session_epoch > chat_targets.session_epoch` (strictly higher; equal epoch is not advance) — reset per-epoch **`sync_state`** watermarks for the **envelope epoch** and accept as a fresh stream **after** decrypt succeeds. **`session_epoch` decrease** remains hard reject.

### Passive adopt transaction (first successful ingest)

When classifier rule 2 applies and the message passes decrypt + D013 (steps 7–11), **step 12 persist** MUST run a **single dual-DB transaction** (D044) that includes the inbound row **and** chat-target epoch adoption **before** any subsequent outbound seq assignment:

| Store | Updates (same txn as message append) |
|-------|--------------------------------------|
| **`profile.db` → `chat_targets`** | `session_epoch = envelope.session_epoch`; `next_outgoing_seq = 1` |
| **`profile.db` → `chat_targets`** (**`rotate_psk`**) | If bundle not yet imported: append `{ epoch: <local before adopt>, master_psk_b64: <previous active>, retired_at }` before setting `session_epoch` (D083). **Preferred path:** innocent peer imports **E020 bundle** at OOB — ledger + `session_epoch` set at import (D086); passive adopt on first ingest only completes watermarks if needed. |
| **`profile.db` → `chat_targets`** (**epoch-only**, D014) | No `retired_psks_json` entry — same `master_psk` covers all epochs via HKDF. |
| **`profile.db` → `threads`** | Denormalized `session_epoch` cache on the direct E2E row (D047). |
| **`thread.db`** | Cancel `relay_visible` `pending`/`failed` rows for **previous** local `session_epoch`; purge matching **`profile.db` `outbox`** rows (D068 — same as local bump). |
| **`thread.db` → `sync_state`** | Initialize fresh watermarks for **`envelope.session_epoch`**; clear `compromised` / `user_resolution` when adoption completes a **`rotate_psk`** recovery. |

**No `profiles/{id}/crypto/sessions.json`** — durable epoch/seq/PSK state is **`chat_targets` only** (D084).

### `rotate_psk` vs epoch-only (innocent peer)

| Initiator path | Innocent peer prerequisite | Adopt trigger |
|----------------|------------------------------|-----------------|
| **Epoch-only** (D014, same PSK) | None — decrypt via `ResolveMasterPskForEpoch(envelope.session_epoch)` + unchanged `master_psk` | First successful ingest at higher epoch |
| **`rotate_psk`** | **Rich OOB bundle installed** (E020/D086) — `master_psk_b64`, merged `retired_psks_json`, and `session_epoch = active_epoch` before decrypt | First successful ingest after bundle install; adopt txn completes seq/outbox/sync if install was partial |

If a **`rotate_psk`** epoch-`N` message arrives before local bundle install → AEAD decrypt fails → **hard reject** (step 7). Cannot decrypt epoch-`N` while still assigning outbound seq against epoch `N-1`. Multi-hop rotation (peer bumped more than once before ingest) is covered by **rich OOB bundle** (D086/E020).

### Contrast with local epoch bump coordinator

| | **Local coordinator** (D014, D068, D083) | **Passive adopt** (D085) |
|---|------------------------------------------|--------------------------|
| Trigger | User **Start new secure chat** / recovery | Peer message at higher `session_epoch` |
| PSK | User supplies new key (`rotate_psk`) or keeps same (epoch-only) | Epoch-only: unchanged; `rotate_psk`: OOB install **before** ingest |
| `chat_targets` | Same column updates | Same column updates |
| Old-epoch pending | Cancel (D068) | Cancel (D068) — peer has moved on |

**Rationale:** Without ingest-triggered adoption, a device could decrypt peer epoch-2 traffic (HKDF with envelope epoch) while `next_outgoing_seq` and outbound envelope `session_epoch` still reflect epoch 1 — breaking seq namespace alignment and risking cross-epoch sends. Colocating adopt with first successful persist keeps decrypt, classifier, outbound assignment, and durable counters consistent.  
**Alternatives:** Lazy adopt on first **outbound** send (rejected — inbound-only window leaves seq/epoch split); background adopt without persist txn (rejected — race with outbox).

---

## D086 — Rich OOB PSK bundle with bounded retired epochs (O006)

**Date:** 2026-07-02  
**Cross-project:** [e2e-message-crypto E020](../e2e-message-crypto/DECISIONS.md#e020--rich-oob-psk-bundle-v1).  
**Decision:** Resolve **O006** (multi-hop `rotate_psk` before innocent peer ingests) with **rich OOB**: rotation export includes the **active** `(epoch, master_psk)` plus up to **`kMaxRetiredPskEpochs` (8)** retired `(epoch, master_psk)` pairs — the **most recent tail** immediately before `active_epoch`. Innocent peer **installs the bundle** (paste / QR) **before** epoch-`N` traffic can decrypt.

### OOB bundle install (innocent peer — `rotate_psk`)

Single **`profile.db` transaction** under writer mutex:

1. Validate bundle (`pp-browser-psk-bundle-v1`, E020); reject if serialized size > **`kMaxPskBundleBytes`** (4 KiB).
2. **Merge** `retired_epochs[]` into `chat_targets.retired_psks_json` by `epoch` (skip duplicates; keep existing entries not in bundle).
3. Set `master_psk_b64`, `psk_fingerprint`, **`session_epoch = active_epoch`**.
4. Reset **`next_outgoing_seq = 1`**.
5. Cancel old-epoch `relay_visible` pending/failed in `thread.db` + matching **`outbox`** rows (D068).
6. Update `threads.session_epoch` denorm; init **`sync_state`** watermarks for `active_epoch`.

After install, first ingest at `active_epoch` uses normal D013 (classifier rule 2 does not fire when epochs match). **Passive adopt (D085)** still runs when `envelope.session_epoch > chat_targets.session_epoch` (epoch-only peer bump without bundle).

### Initiator export (after local `rotate_psk`)

Export **`pp-browser-psk-bundle-v1`** with:

- `active_epoch`, `master_psk_b64` (new active key)
- `retired_epochs[]` = up to **`kMaxRetiredPskEpochs`** entries from local ledger with epochs in **`(active_epoch - K .. active_epoch - 1]`** (most recent tail). Include the epoch just retired if not already in ledger.

If peer's last known epoch is older than the tail window, **disclose** that relay ciphertext from skipped intermediate epochs may be **permanently undecryptable** on this device.

### Ledger cap on disk

`retired_psks_json` MUST NOT exceed **`kMaxRetiredPskEpochs`** entries after merge or local `rotate_psk` append — **prune lowest epoch numbers** first (E018 hygiene aligned with bundle cap).

### Scope

| Case | Mechanism |
|------|-----------|
| **Initial PSK** (epoch 1, no prior rotation) | Generate + export raw base64 on one peer; import on other (E011); or bundle with empty `retired_epochs[]` |
| **Single-hop `rotate_psk`** | Bundle with one retired epoch — full backfill within window |
| **Multi-hop `rotate_psk`** before ingest | Bundle retired tail — all epochs in window decryptable; older relay ciphertext outside window not guaranteed |
| **Epoch-only bump** (D014) | Unchanged — same `master_psk`; no bundle required; HKDF re-derives per epoch |

**Rationale:** Retired ledger alone cannot cover peer-initiated multi-hop rotation unless OOB carries the skipped keys; a bounded tail keeps paste/QR size predictable (`~1 KiB` at K=8) while fixing the common 1→2→3 offline gap.  
**Alternatives rejected:** O006-A round-trip gate only (does not fix zero-message double-rotate); O006-C defer; unbounded retired history (storage + OOB size).

---

## D087 — Binary `ChatPayload` v1 (E014 body_hash + E010 plaintext)

**Date:** 2026-07-02  
**Updated:** 2026-07-02 — flattened type tail + pp Binary Wire Profile (D088).  
**Decision:** Message body semantics use **binary `ChatPayload` v1** per [WIRE_SCHEMAS § ChatPayload](WIRE_SCHEMAS.md#chatpayload-v1--binary-d087d088) and [§ Wire profile](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088):

| Path | Encoding |
|------|----------|
| E2E wire | **`body.e2e.payload_b64`** — ciphertext blob; plaintext inside AEAD |
| E014 `body_hash` | `body_kind=0x02` \|\| decoded E2E blob bytes (D090) |
| E010 AEAD plaintext | Binary `ChatPayload` v1 |
| Disk (`messages.chat_payload`) | Raw BLOB — same bytes as E2E decrypt output |

**Layout (v1):** `payload_version`, `content_type`, `text` (**LenUtf8**), inline **type tail** (no opaque `typed_payload` blob). **`schema_version`** byte removed — use **`payload_version`** only.

**Implementation:** **`ChatPayloadCodec`** via **`WireLenUtf8`** + **`OutputArchive`** / **`binaryPack`** ([`Serialize.hpp`](../../src/common/Serialize.hpp), [`BinaryPack.hpp`](../../src/common/BinaryPack.hpp)).

**Rationale:** Relay is greenfield (D016); binary matches crypto formats; one profile (D088) across payload, AAD, and sign strings.  
**Alternatives:** JSON body (rejected); opaque typed blob with mixed u16/u32 lengths (rejected — D088).

---

## D088 — pp Binary Wire Profile (LenUtf8 / LenBytes)

**Date:** 2026-07-02  
**Decision:** All in-tree **binary** wire formats share one encoding profile documented in [WIRE_SCHEMAS § Wire profile](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088):

| Construct | Rule |
|-----------|------|
| **LenUtf8** | `u64` BE count + UTF-8 bytes — C++ **`WireLenUtf8`** |
| **LenBytes** | `u64` BE count + opaque bytes — C++ **`WireLenBytes`** |
| **Integers** | Fixed-width big-endian |
| **Fixed raw** | No length prefix (`std::array<uint8_t, N>`) |
| **Decode** | Exact consume; reject trailing bytes; enforce D029 caps at codec |
| **Forbidden** | Maps/sets/floats/pointers/JSON inside signed or hashed bytes |

**Applies to:** ChatPayload (D087), E2E AAD string fields (E004), E014 sign-string fields, E2E ciphertext tail (**LenBytes** after fixed nonce).

**Reference implementation:** [`src/common/Serialize.hpp`](../../src/common/Serialize.hpp), [`src/common/BinaryPack.hpp`](../../src/common/BinaryPack.hpp) (`pbr::` namespace, **`Roe<T>`** errors).

**Rationale:** One length-prefix rule eliminates u16/u32/u64 drift; aligns C++ archive with normative bytes; same class of bug as JSON canonicalization.  
**Alternatives:** Per-format length widths (rejected); raw `std::string` on wire (rejected — ambiguous).

---

## D089 — Three chat tiers; both direct tiers E2E (E021)

**Date:** 2026-07-02  
**Updated:** 2026-08-15 — public rotation: account-scope no auto-`rotate_psk`; device-lock / D2D auto-rekey (**D101** / **E027**).  
**Cross-project:** [e2e-message-crypto E021](../e2e-message-crypto/DECISIONS.md#e021--three-chat-tiers-both-direct-tiers-e2e-d089), [E022](../e2e-message-crypto/DECISIONS.md#e022--group-e2e-pairwise-sender-keys), [E023](../e2e-message-crypto/DECISIONS.md#e023--no-public_relay-wire-value-d090).  
**Amends:** D004, D013, D018, D045, D046, D062, D080, D076, D063; **no `public_relay`** (D090).  
**Decision:** Product P2P chat has **three tiers**. **All three** use symmetric E2E body encryption on the wire (relay sees ciphertext). They differ by **priority** and **policy defaults**, not by whether bodies are encrypted.

| Tier | `Thread.kind` | Wire `route.channel` (direct) | UI label | Priority |
|------|---------------|----------------------------------|----------|----------|
| **Private direct** | `direct` | `e2e` | Private | Security first; maximize UX only where security is not compromised |
| **Public direct** | `direct` | `e2e_public` | Public | UX first; accept security tradeoffs when they conflict with fluency |
| **Group** | `group` | *(none — `route.kind=group`)* | Group | UX first; **pairwise sender-keys** preferred over a single group PSK (E022) |

**Direct `ChatTargetKey`:** `(peer_identity_kind, peer_identity_value, channel)` where `channel` ∈ `{ e2e, e2e_public }`. Same communicating identity may have **two unrelated direct threads** (separate PSK, seq, memory) — extends D004.

**Policy profiles** (see [DESIGN § Three chat tiers](DESIGN.md#three-chat-tiers-d089)):

| Dimension | Private (`e2e`) | Public (`e2e_public`) | Group |
|-----------|-----------------|------------------------|-------|
| Key establishment | Manual OOB PSK + mandatory fingerprint (E011) | Hybrid KEM PSK + signing resolver (E013/E024) | Auto pairwise keys on join (E022) |
| Ingest on seq conflict | Strict D013; pause + rotate or pause only (D038) | Relaxed default: `continue_anyway` / LWW (D046 rules) | Same as public direct |
| Multi-device | Unsupported v1 → compromise (D015) | Target: supported (D074 extension) | Target: supported |
| Inbound shell | Find-only; reject without row (D062) | Auto-create thread + keys on first message | Auto on invite/join |
| Key rotation | User-driven; recommend on compromise | Account-scope: no auto-`rotate_psk` (E027). User **Use only this device…** then D2D auto-rekey when both locked (D101) | Pair-key rotation on membership change |
| Retired PSK ledger | Cap 8 epochs (D086) | Higher cap or longer retention (product tuning) | Per-pair ledgers |

**Wire (direct):** **`e2e`** and **`e2e_public` only** — see [D090](#d090--no-public_relay--plaintext-direct-wire). **Message** → `e2e_public`; **Secure message** → `e2e`.

**Phasing:** **private direct** (`e2e` strict), **public direct** (`e2e_public` auto-key), and **group** (E022) are in product.

**Rationale:** Users expect “public” chat to be convenient, not relay-readable plaintext; security-sensitive users still get a strict tier.  
**Alternatives:** Plaintext `public_relay` (rejected — D090); single direct tier with security slider (rejected).

---

## D090 — No `public_relay` / plaintext direct wire

**Date:** 2026-07-02  
**Amends:** D063, D089, E021; supersedes all **`public_relay`** / **`body.content_b64`** direct paths.  
**Decision:**

| Rule | Behavior |
|------|----------|
| **Direct `route.channel`** | **`e2e`** \| **`e2e_public` only** — reject `public_relay` and unknown values on ingest |
| **Direct `body`** | **`{ "e2e": { "payload_b64": "…" } }` only** — no `body.content_b64` on direct envelopes |
| **`sender_seq` / `session_epoch`** | Required on wire for both direct tiers (D045) |
| **Receive pipeline** | All direct messages run decrypt (steps 7–11) — no plaintext bypass branch |
| **Legacy data** | Pre-cutover envelopes / dev data — **reject** or wipe per D016; no dual-parser |

**Rationale:** Greenfield protocol (D016); three-tier model has no plaintext direct tier; one envelope shape simplifies implementer constraints.  
**Alternatives:** Bootstrap shim until `e2e_public` (rejected — user chose no legacy support).

---

## D091 — Blockchain contact id (CAIP-10, E024)

**Date:** 2026-07-02  
**Cross-project:** [e2e E024](../e2e-message-crypto/DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007).  
**Decision:** When **`ContactIdKind::Blockchain`** (D079, `ContactTypes.h`), the **`value`** string MUST be a [CAIP-10](https://github.com/ChainAgnostic/CAIPs/blob/master/CAIPs/caip-10.md) account id:

```
eip155:{chain_id}:{address}
```

| Part | Rule |
|------|------|
| Namespace | **`eip155`** for EVM chains (CAIP-2 `eip155` + CAIP-10 account) |
| `chain_id` | Decimal string per CAIP-2 (e.g. `1` = Ethereum mainnet, `8453` = Base) |
| `address` | Lowercase hex with **`0x`** prefix (EIP-55 checksum **not** required on wire/storage) |

**Examples:** `eip155:1:0xabc123…`, `eip155:8453:0xdef456…`

**Scope v1:**

| Use | Allowed |
|-----|---------|
| **`Contact.ids[]`** metadata, people search, UI | yes — first-class **lookup** handle (find Peer ID; see [D096](#d096--identity-roles-peer-id-who-caip-10-find-relay-route)) |
| On-chain attestation linking **CAIP-10 ↔ Peer ID ↔ signing key** (and optional relay id) | **`[later]`** (E024 Anchor 1) |
| **`ChatTargetKey.peer_identity_value`** / wire **`sender_contact_id`** | **no** — remains `relay:…` (D082) until deliberate protocol bump; not CAIP-10 |
| PSK / hybrid KEM | **no** — blockchain attests signing keys only; PSK auto-key uses **account** KEM (E024 / **M015**) |

Non-EVM chains: add new **`ContactIdKind`** or a namespaced prefix in a future decision — do not overload `eip155:` with non-EVM semantics.

**Rationale:** CAIP-10 is the standard hook for on-chain identity **search and attestation** without making the wallet the dialable or wire identity.  
**Alternatives:** Raw `0x` address only (rejected — ambiguous chain); ENS/DID strings in `Blockchain` kind (deferred — resolver layer); blockchain as wire identity in v1 (rejected — E024 / D096).

---

## D092 — Release scope bucket B

**Date:** 2026-07-06  
**Status:** **Amended by [D100](#d100--release-scope-b-pq-account-id)** for identity / PQ / Account ID wire. Chat v2a–v6 + private E2E polish from Bucket B still apply.  
**Decision (historical):** First customer release = **v1 + post-v1 polish** (PHASES scope bucket **B**): chat **v2a–v6** + [e2e c1–c3](../e2e-message-crypto/PHASES.md) (private `e2e` tier) **plus** post-v4, post-v6b/c/d. Originally excluded: `e2e_public` auto-key (c3+), group E2E, PQ (c4).  
**Rationale:** Ship core E2E + sync with additive polish phases without opening public tier or group wire work.  
**Alternatives:** Bucket A (minimal private only); C/D (public tier or PQ — deferred).  
**Amended by:** [D099](#d099--account-id-amends-d096-multi-device), **[D100](#d100--release-scope-b-pq-account-id)**.

---

## D093 — Relay backend for v6-sync (D027)

**Date:** 2026-07-06  
**Decision:** **External relay D027 is ready** for v6-sync exit validation. Client ships `HttpRelayClient::FetchChatHistory` (signed `POST …/v1/streams/messages/query`) and `MockRelayClient` for dev/CI when relay `base_url` is unset. v6-sync integration tests may use **live relay**; mock-only exit is **not** required.  
**Rationale:** Relay service and client contract are both available — unblock v6-sync without waiting on mock-only criteria.  
**Alternatives:** B — mock-only until relay ships (superseded); C — in-repo relay stub (not needed).

---

## D094 — Peer-direct history required for v1 (D060)

**Date:** 2026-07-06  
**Product override:** v1 release **requires** libp2p peer-direct **`/pp-browser/chat-history/1.0.0`** (wave **v6-libp2p** / PHASES 4d). Relay D027 remains **fallback** when peer is offline or direct fails (D058 transport order unchanged).  
**Rationale:** “Sync with peer” (D059) must exercise direct transport in v1, not relay-only.  
**Alternatives:** A — relay-only for v1 (superseded — peer-direct deferred to post-v1).

---

## D095 — Group pairwise wire shape (O008)

**Date:** 2026-07-06  
**Cross-project:** [e2e E022](../e2e-message-crypto/DECISIONS.md#e022--group-e2e-pairwise-sender-keys).  
**Decision:** Group outbound wire uses **N ciphertexts per message** — the sender includes **one AEAD ciphertext per member**, each encrypted with that member's pairwise secret (reuse 1:1 `ChatTargetKey` crypto). **Not** a sender-keys tree; **not** relay-side encrypted fan-out in v1 slice. Normative detail when group ships: extend `RelayEnvelope` / group send path with a member→ciphertext map (exact JSON field names TBD at implementation).  
**Rationale:** Matches E022 pairwise sender-keys policy; simplest fan-out on existing pair machinery.  
**Alternatives:** Sender-keys tree (deferred); relay encrypted fan-out (deferred).

---

## D096 — Identity roles: Peer ID (who), CAIP-10 (find), relay (route)

**Date:** 2026-07-09  
**Updated:** 2026-07-09 — clarify chain vs peer vs relay roles.  
**Updated:** 2026-08-11 — **amended by [D099](#d099--account-id-amends-d096-multi-device)** for multi-device account root (pre-release). Until m1–m2 land, **code still matches the single-device table below**.  
**Amends UX of:** D082 (relay-user string remains wire format).  
**Cross-project:** [D091](#d091--blockchain-contact-id-caip-10-e024), [e2e E024](../e2e-message-crypto/DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007), [multi-device-account](../multi-device-account/).  
**Decision:**

Identity strings serve different verbs — do not treat them as interchangeable “user ids”:

| Role | Kind / value | First-class for | Not for |
|------|----------------|-----------------|---------|
| **Network identity (who)** | libp2p **Peer ID** (base58) | Product self-id, dial/bind, Me primary, future `peer_id` threads | — |
| **Lookup / social handle (find)** | **CAIP-10** blockchain account (`ContactIdKind::Blockchain`) | People search, link-to-peer resolution, on-chain attestation → Peer ID + signing key | Dialing; v1 wire `sender_contact_id` |
| **Transport / registry handle (route)** | **`relay:<opaque_id>`** | v1 relay inbox, directory by relay id, v1 `ChatTargetKey` | Product primary id |

1. **Peer ID:** Derived from the Ed25519 signing public key (same key as registration + envelope signatures). Held on `LocalIdentity.peer_id` as an **in-memory** cache only — **not** written to `identity.json` (pubkey is the durable source). Me settings: primary line; Copy / Share use Peer ID.
2. **CAIP-10:** First-class **lookup** handle — resolve `eip155:{chain_id}:{address}` → Peer ID (+ signing pubkey, optional relay id, multiaddrs). Attestation strengthens signing trust (E024 Anchor 1); does **not** become wire identity in v1 (D091).
3. **Relay ID:** Registration-assigned (D082), empty until register succeeds; secondary on Me. Lookup path into Peer ID / keys via relay/directory — **not** synthesized from a pubkey prefix.
4. **v1 wire unchanged:** `ChatTargetKey` / `sender_contact_id` / AAD continue to use `peer_identity_kind = relay_user` and `relay:…` until a deliberate protocol bump to `peer_id` threads (D079). Target directory map: `CAIP-10 | nickname | relay_id → Peer ID`.
5. **One keypair:** The Ed25519 key in `identity.json` is the source for Peer ID derivation, message signing, and registration proofs.

**Slogan (single-device era):** Peer ID = who. Chain account = how you find who. Relay = how messages route today.

**Rationale:** Dialable P2P identity exists before relay registration; wallet search and attestation must not be confused with the network id or the v1 transport account.  
**Alternatives:** Keep relay-first Settings (rejected); CAIP-10 as Me primary / wire id in v1 (rejected — D091); flip wire to Peer ID in the same change (rejected — protocol bump).

**Superseded in part by D099** for multi-device: person = Account ID; Peer ID = device endpoint; relay = route binding; account key signs (not one keypair for all roles). Canonical matrix: [multi-device-account DESIGN](../multi-device-account/DESIGN.md).

---

## D097 — Shared mesh host + on-demand session policy + direct chat protocol

**Date:** 2026-07-09  
**Decision:**

1. One shared `Libp2pHost` (Yamux + Noise) owned by `ConversationsHub`; history and direct chat register protocol handlers on it.
2. `PeerSessionManager` implements on-demand dial, ConnectionManager reuse, warm-active set, idle TTL, connection caps, dial backoff — **not** an app-level socket pool.
3. Direct live messaging uses `/pp-browser/chat/1.0.0` (length-prefixed JSON `RelayEnvelope`); send path is direct-first then relay fallback; ingest via `RelayReceivePipeline` with `MessageTransport::Direct`.
4. v1 thread routing remains relay-id (`ChatTargetKey`); PeerId is used for dial via multiaddr `/p2p/…`. Contacts persist `multiaddrs`.
5. Mobile background: suspend cold peer connections; keep warm (active-thread) set.

**Rationale:** Aligns with cpp-libp2p Dialer reuse; scales to many contacts without holding all connections; keeps relay as offline path.  
**Alternatives:** App connection pool (rejected); always-on mesh to all contacts (rejected); PeerId-native threads in same change (deferred — D096).

---

## D098 — Reaction annotations (`reaction` / `reaction_clear`)

**Date:** 2026-08-08  
**Decision:**

1. Message reactions use existing `content_type=annotation` rows (D005/D026) — not in-place mutations of the target.
2. **`value` is a plain UTF-8 emoji string** (codec/tests). Documentary nested `{ "emoji": "…" }` shapes are rejected.
3. **Add:** `annotation_type=reaction`, `text` = display glyph, `value` = emoji, `target_message_id` set. Store original UTF-8; compare with `NormalizeEmojiKey` (strip trailing `U+FE0F`).
4. **Remove (toggle off):** append `annotation_type=reaction_clear` with the same `target_message_id` + `value` emoji key and empty `text`. Display keeps the **latest** row per `(sender_contact_id, target_message_id, emoji_key)`; clear wins.
5. Both types count toward **`kMaxAnnotationsPerTarget`** (D042); enforce on compose and ingest persist.
6. Mobile: OS emoji keyboard for composer text; reaction UX uses a fixed preset strip + **More…** opening the in-app emoji picker (curated catalog; recently used in profile prefs). Composer ☺ opens the same picker for insert.

**Rationale:** Reuses the shipped annotation path; append-only sync stays simple; toggle without mutating history.  
**Alternatives:** Mutate target row likes array (rejected — D005); nested JSON `value` object (rejected — codec is string); full Unicode dump (rejected — curated catalog + OS paste for rare glyphs).

**Follow-up (2026-08-10):** In-app emoji catalog is no longer deferred for insert / reaction **More…** — see `EmojiCatalog` + `EmojiPickerController`.

---

## D099 — Account ID amends D096 (multi-device)

**Date:** 2026-08-11  
**Updated:** 2026-08-13 — D015 is one *sender* not one device ([M016](../multi-device-account/DECISIONS.md#m016--dogfood-one-active-sender-on-linked-devices)).  
**Amends:** [D096](#d096--identity-roles-peer-id-who-caip-10-find-relay-route) (person/endpoint/route split; ends “one keypair” for multi-device).  
**Does not amend:** D082 string rules for `relay:` **as a route id**; D091 (CAIP-10 find-only).  
**Canonical spec:** [multi-device-account](../multi-device-account/) ([DESIGN](../multi-device-account/DESIGN.md), [M001–M019](../multi-device-account/DECISIONS.md)).  
**Cross-project:** [e2e E025](../e2e-message-crypto/DECISIONS.md#e025--account-envelope-signing--private-psk-not-auto-synced), [at-rest A010](../at-rest-crypto/DECISIONS.md#a010--shared-dek-per-device-vault-wrap-multi-device).

**Decision:**

| Role | Value | Scope |
|------|--------|-------|
| **Account (person)** | `account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 pk))>` | Shared across linked devices |
| **Endpoint (install)** | libp2p **Peer ID** from **device** keypair | Per device |
| **Route** | `relay:<opaque_id>` | Per relay server binding |
| **Find** | CAIP-10 (optional) | Alias → Account ID |

1. **Account ID** is the communicating-identity target for the pre-release hard cut (`ChatTargetKey` / wire) — see multi-device-account M007; code remains `relay_user` until m2.
2. **Contact / wire:** `ContactIdKind::Account` / `peer_identity_kind=account` ([M009](../multi-device-account/DECISIONS.md#m009--contactidkindaccount-wire-peer_identity_kindaccount)); envelopes/AAD vs relay auth ([M010](../multi-device-account/DECISIONS.md#m010--envelopeaad--account-id-relay-api-auth-stays-relay)).
3. **Peer ID** is no longer the account “who”; it is the mesh endpoint for one install (avoids dual-online conflict).
4. **Brief register binding:** prove account key → at most one `relay_user_id` per Account ID per server (M006). Directory Account-first early ([M011](../multi-device-account/DECISIONS.md#m011--brief-directory-account-first-by-account-lookup-search-q-matches-account-id)).
5. **Signing / PSK sync / DEK:** owned by e2e E025 and at-rest A010 — not re-specified here.
6. D015 is **one sender**, not one device: linked installs may receive ([M016](../multi-device-account/DECISIONS.md#m016--dogfood-one-active-sender-on-linked-devices)). D074 `sender_instance_id` remains until a multi-writer phase.

**Rationale:** Portable multi-device account without overloading Peer ID or `relay:` as the person root; destructive cut acceptable pre-release.  
**Alternatives:** Keep D096 as-is; soft-cut wire stays `relay:`; account = CAIP-10.

---

## D100 — Release scope = Bucket B + PQ + Account ID (amends D092)

**Date:** 2026-08-13  
**Updated:** 2026-08-13 — link-device paste (**M012** m4b) landed; remaining multi-device follow-ups live in that project (**M016–M019**).  
**Amends:** [D092](#d092--release-scope-bucket-b).  
**Cross-project:** [multi-device-account M007–M019](../multi-device-account/DECISIONS.md); [e2e-message-crypto](../e2e-message-crypto/) aggressive PQ path.  
**Decision:** Pre-release messaging/identity scope is **Bucket B plus**:

| Include (with B) | Still deferred (release gates / later) |
|------------------|----------------------------------------|
| Aggressive PQ account identity (ML-DSA-65 register/API auth, ML-KEM-768 auto-key) | **`e2e_public` send** enablement (keys/trust may land earlier) |
| Account ID on wire, catalog, AAD ([M009](../multi-device-account/DECISIONS.md#m009--contactidkindaccount-wire-peer_identity_kindaccount), [M010](../multi-device-account/DECISIONS.md#m010--envelopeaad--account-id-relay-api-auth-stays-relay)) | Cloud message sync; hard-delete “forget forever”; remote wipe |
| Brief Account-first directory early ([M011](../multi-device-account/DECISIONS.md#m011--brief-directory-account-first-by-account-lookup-search-q-matches-account-id)) | Dual-writer D074; unlink KEM rotation ([M019](../multi-device-account/DECISIONS.md#m019--unlink-local-forget-kem-rotation-is-revoke) phase 2) |
| Link-device paste + shared DEK ([M012](../multi-device-account/DECISIONS.md#m012--link-device-ritual-deferred-until-m4) m4b) | Live sibling chat-index refresh (not a second paste) |

Chat v2a–v6 + private E2E polish from Bucket B remain in scope. Multi-device **m3 `endpoints[]`** and **m4c** contacts-in-paste are tracked in [multi-device-account PHASES](../multi-device-account/PHASES.md), not as chat-storage gates.

**Rationale:** Identity hard-cut is in-scope for the same release as local chat storage; public auto-key *send* stays gated.  
**Alternatives:** Keep D092 exclude of PQ/multi-device entirely; ship wire cut without Brief by-account API.

---

## D101 — Public `key_scope`, `psk_rotate` ingest, and rotation policy

**Date:** 2026-08-15  
**Status:** Accepted.  
**Amends:** [D089](#d089--three-chat-tiers-both-direct-tiers-e2e-e021) rotation row; [D084](#d084--psk-columns-on-chat_targets-in-profiledb-e008) columns; two PSKs at the same epoch are a hard crypto failure — not [D046](#d046--relaxed-ingest-tier-default-for-public-direct-strict-only-in-v1-private) LWW.  
**Cross-project:** [E027](../e2e-message-crypto/DECISIONS.md#e027--public-11-device-lock-rekey-auto-rotate_psk-only-when-both-sides-are-device-bound), [M020](../multi-device-account/DECISIONS.md#m020--device-scoped-public-psks-stay-off-the-link-bundle), [D085](#d085--passive-epoch-advance-peer-bumps-first).

**Decision:**

1. Additive `chat_targets` columns (`profile.db` `PRAGMA user_version` 3):
 - `key_scope` TEXT NOT NULL DEFAULT `'account'` — `account` | `device_self` | `device_pair` | `locked_out`
 - `thread_kem_pk_b64` / `thread_kem_sk_b64` (local conversation ML-KEM-768; sk DEK-wrapped like `master_psk_b64`)
 - `peer_thread_kem_pk_b64`
 - `last_psk_rotate_at` INTEGER (unix ms)
 - `psk_rotate_msg_count` INTEGER NOT NULL DEFAULT 0 (messages in current epoch toward the auto-rekey watermark)
2. Inbound `control_type=psk_rotate` on `e2e_public` only. Decrypt the payload with the **current** PSK first. Then verify `key_init_hash`, decapsulate `key_init` with the secret named by `wrap_kind`, install the new `master_psk` + epoch (D083 retired ledger + D085 adopt), store initiator `thread_kem_pk_b64` as `peer_thread_kem_pk_b64`. Do **not** treat this `key_init` as first-message auto-key (`ResolveOrDeriveMasterPsk` stays “PSK missing” only).
3. **Scope transitions:**
 - This install initiates lock: `account` → `device_self` (or `device_self` → `device_pair` if `peer_thread_kem_pk` already set).
 - Peer lock received and `key_init` opens: if local already `device_self`, go `device_pair`; else stay `account` (peer locked themselves; we still have account-scope access).
 - `key_init` fails after a valid notice: `locked_out`. Compose disabled. Banner. Old local history stays (D048). Old-epoch relay ciphertext still decrypts via retired ledger.
4. Two different PSKs at the same `session_epoch` = **hard crypto failure** on all tiers — not D046 LWW.
5. D080 public auto-create is unchanged for account-scope first messages.
6. Unlock / restore to account scope is out of this slice.

**Rationale:** Scope and conversation KEM must live next to the PSK (same `ChatTargetKey` row) so link-device, ingest, and the thread menu share one source of truth.  
**Alternatives:** New `route.channel` (rejected — would fork another thread identity); store conversation KEM only in memory (rejected — must survive restart).

---

## Open decisions (not yet resolved)

None for this project’s local-storage checklist (resolved 2026-07-06). Identity / PQ / Account ID ADRs live in [multi-device-account/DECISIONS.md](../multi-device-account/DECISIONS.md) and [e2e-message-crypto/DECISIONS.md](../e2e-message-crypto/DECISIONS.md).

**Resolved:** **O007** → [e2e E024](../e2e-message-crypto/DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007); **O008** → D095; release scope → **D100**.
