# Design — complete system specification

Authoritative **what** for chat storage, memory, and P2P messaging channels. **Implementation order:** [PHASES.md](PHASES.md). **Rationale:** [DECISIONS.md](DECISIONS.md). **Gap vs code today:** [CURRENT_STATE.md](CURRENT_STATE.md).

## How to read this document

| Tag | Meaning |
|-----|---------|
| **`[v1]`** | First shipping slice (phases v2a–v6 in [PHASES.md](PHASES.md)) |
| **`[post-v1]`** | Planned extension; spec is stable — enable when product prioritizes (post-v4, post-v6*, etc.) |
| **`[future]`** | Optional / TBD; not scheduled |

Do not duplicate behavior specs in PHASES — phases link here. DECISIONS records **why** choices were made; if DECISIONS and this doc disagree on behavior, **this doc wins**.

### Maturity at a glance

| Area | `[v1]` | `[post-v1]` |
|------|--------|-------------|
| ChatPayload wire types | `text`, `system` | `annotation`, `contact_card`, `crypto_tx` |
| `@ai` in direct threads | local `@ai` | `@ai+`, `@ai++` shared modes |
| P2P sync (E2E) | tail + gap repair | scroll-driven history backfill |
| Integrity recovery | rotate PSK or pause | relaxed ingest / continue anyway |
| Sidebar | flat list + channel badge | optional grouped sections |
| Transport | persist `transport` column | per-message badge UI |
| Ingest | E2E strict D013; public UUID dedup | (unchanged model) |

## Implementer constraints

When building **`[v1]`** phases, satisfy these so **`[post-v1]`** features plug in without schema migration or hot-path refactors:

| Rule | Why |
|------|-----|
| **Branch on `content_type`**, default `text` | Rich types add templates, not a new storage model |
| **Branch on `thread.channel`** for seq/sync | E2E → D013 + `sender_seq`; public → UUID + timestamp (D045) |
| **Reject unknown `content_type` on relay ingest only** | Wire validator; local rows may use future types in dev |
| **Keep all `messages` columns** at schema creation | `target_message_id`, `generation`, `transport`, seq fields — nullable until used |
| **Populate full `sync_state` watermarks in v6** | `loaded_min_seq` / `loaded_max_seq` needed for `[post-v1]` history backfill |
| **Implement `GetMessagesBySeqRange` in v6** | Tail, gap, and history backfill share one query |
| **`sync_state.state_json` extensible** | `[post-v1]` relaxed ingest adds keys without DB bump |
| **Set `transport` at send/receive** | `[post-v1]` badge UI reads column |
| **Participant check on all inbound direct** | D027 auth model |
| **Do not hardcode “AI never relays” in store layer** | Shared `@ai` sets `relay_visible=true` on specific rows only |

## Principles

1. **One transcript model** — UI, disk, and LLM context derive from the same message store (`ThreadMessage` / future extensions), not parallel in-memory shapes.
2. **Local source of truth** — write locally before send; relay rejections do not erase history ([P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md)).
3. **Three storage layers** — distinguish what the user sees, what fits in the LLM window, and what the agent remembers long-term.
4. **Channel isolation** — public (relay) and E2E conversations with the same contact are different threads with different memory boundaries.
5. **Stable IDs everywhere** — messages, threads, and annotation targets use UUIDs for dedup, sync, and reactions.
6. **Sender sequence for E2E completeness** — in **`e2e` direct threads**, peer-visible messages carry a per-sender monotonic `sender_seq` (in addition to UUID) so receivers detect gaps in the live tail. **`public_relay`** uses UUID dedup + timestamp ordering only (D045). UUID remains the only message identity everywhere.
7. **Strict ingest on E2E only** — **`e2e` direct** threads use D013: **normal**, **gap**, **soft compromised**, or **hard reject**. Soft failures **pause** ingest and outbound and show a **choice sheet** with recommended recovery only (D038, D046) — no “continue anyway” / relaxed ingest in v1. **`public_relay`** accepts any signed message from a participant with UUID dedup; no seq classifier.
8. **Durable outbox** — `relay_visible` rows with `delivery=pending` or `failed` survive app restart; retries reuse the same `(message_id, sender_seq)` on E2E (D017); public relay retries reuse `message_id` only.
9. **Storage abstraction** — `IThreadStore` stays the seam; **`SqliteThreadStore`** per-thread `thread.db` + `threads/profile.db` (`threads` catalog + `outbox` + `chat_targets`, D028, D035, D036, D047) from v2a. No `index.json` or other JSON thread files.

## Assumptions (v1)

| Assumption | Implication |
|------------|-------------|
| **Single active sender per identity** (D015) | One client per profile may send on an **E2E** chat target at a time. Two devices with the same identity and PSK without coordination will emit conflicting `sender_seq` → compromised (D011). Document in UX; multi-device seq coordination is out of scope for v1. Public relay is unaffected. |
| **No legacy thread migration** (D016) | Legacy flat `threads/{id}.json` is not upgraded. Schema bumps may require wiping `{data_dir}/profiles/{id}/threads/` (acceptable — no production users yet). |
| **No encryption at rest** (D048) | `thread.db` and `profile.db` are plaintext SQLite on disk. E2E body confidentiality is on the wire only; local disk is trusted. SQLCipher / OS keychain for transcript encryption is out of scope for v1. |
| **Timestamps are display-only for ingest** | `timestamp` is not authoritative for ordering or replay on either channel; E2E uses `sender_seq`; public relay uses arrival order + UUID tie-break. No clock-skew rejection in v1. |

## Three layers (transcript vs context vs memory)

```
┌─────────────────────────────────────────────────────────────┐
│ Layer 1: UI transcript                                      │
│   ThreadMessage[] on disk — full history user can scroll    │
├─────────────────────────────────────────────────────────────┤
│ Layer 2: LLM context window                                 │
│   IContextPolicy / ThreadContextPolicy — sliding trim       │
│   rebuilt each turn; not a user-facing “store”              │
├─────────────────────────────────────────────────────────────┤
│ Layer 3: Durable agent memory                               │
│   ConversationSummary + optional fact store per thread      │
│   survives trimming; cleared independently of transcript    │
└─────────────────────────────────────────────────────────────┘
```

Tool-call scratch (`turn_scratch`) remains **ephemeral per turn only** — never persisted as chat bubbles.

## Data model (target)

### Thread

| Field | Type | Notes |
|-------|------|-------|
| `id` | UUID | Stable; used in relay envelope |
| `kind` | `ai` \| `direct` \| `group` | Unchanged |
| `channel` | `public_relay` \| `e2e` | **New.** Replaces overloading `encrypted` alone |
| `participant_contact_ids` | string[] | One peer for direct |
| `title`, `preview`, `updated_at`, `unread_count` | — | Sidebar metadata; cached in `profile.db` `threads` (D035); preview/`updated_at` derived from `thread.db` on visible-row verify |
| `encrypted` | bool | Derived from `channel == e2e` (keep for UI binding) |
| `session_epoch` | uint32 | **New.** Bumped on E2E key rotation, compromise recovery, device reset, or “new secure chat”; scopes `sender_seq` streams |

**Thread identity for direct:** `(contact_id, channel)` — never one thread for both modes.

### Chat target (long-lived, direct P2P)

Outbound sequence counters and session epochs are keyed to **chat target** `(contact_id, channel)`, not `thread_id`. A thread file may be deleted and recreated for the same person; seq continues until `session_epoch` rotates.

| Field | Scope | Notes |
|-------|-------|-------|
| `next_outgoing_seq` | chat target | Monotonic uint64 per epoch; assigned at first local persist before send |
| `session_epoch` | chat target / thread | Increment on compromise recovery, full device reset, or explicit new secure chat; **only** way to restart seq from 1 (D014) |

Persist in **`profile.db` → `chat_targets`** (D047), updated under the same writer mutex as `outbox`. **`Thread.session_epoch`** is a denormalized cache for UI; **`chat_targets.session_epoch`** is authoritative for outbound seq; e2e **`sessions.json`** must match after epoch bump (see § Epoch bump transaction).

### Per-peer sync state (per thread, scoped by `session_epoch`)

Sync watermarks are keyed by **`(peer, session_epoch)`**. A new epoch starts a fresh stream; old-epoch state is retained for history display but not used for gap logic on the new epoch.

| Field | Notes |
|-------|-------|
| `contiguous_peer_seq[peer][epoch]` | Highest seq received with no holes in the active tail for this epoch |
| `loaded_min_seq[peer][epoch]`, `loaded_max_seq[peer][epoch]` | Oldest/newest peer seq present in local transcript for this epoch |
| `history_floor_seq[peer][epoch]` | Set on **clear visible history** — seq at or below this in the same epoch is **excluded from sync** (D037): silent discard, not compromised |
| `sync_state` | `ok` \| `gap` \| `compromised` — E2E only; `compromised` means paused pending user resolution (D038) |
| `user_resolution` | `null` \| `rotate_psk` \| `reset_thread` \| `pause_only` |
| `integrity_incidents[]` | `{ kind, detected_at, detail }` — ring buffer, max **`kMaxIntegrityIncidents`** (10, D049) per `(peer, epoch)` |

### ThreadMessage

| Field | Type | Notes |
|-------|------|-------|
| `id` | UUID | Client-generated; dedup on ingest |
| `thread_id` | UUID | |
| `content_type` | enum | `text`, `system` **`[v1]`**; `annotation`, `contact_card`, `crypto_tx` **`[post-v1]`** |
| `payload` | JSON object | Type-specific structured body (wire + disk) |
| `text` | optional string | Display snippet / search / plain fallback; AI raw for `text` turns |
| `content_rml` | optional string | Rendered blocks (AI assistant) |
| `user_payload` | optional string | LLM-only structured JSON (AI turns) |
| `chat_actions` | array | Indexed chips |
| `target_message_id` | optional UUID | For `annotation` (and edits referencing prior message) |
| `sender_contact_id` | string | `local:self`, `ai:assistant`, or contact id |
| `timestamp` | int64 | |
| `relay_visible` | bool | `true` when sent to peer; see `@ai` modes |
| `delivery` | enum | local, pending, relayed, failed |
| `transport` | enum | local, relay, direct |
| `control_type` | optional string | For `content_type=system`; reserved for future control rows |
| `sender_seq` | optional uint64 | E2E + `relay_visible=true` only (D045) |
| `session_epoch` | optional uint32 | Must match envelope |
| `generation` | optional enum | `user` \| `ai_on_behalf` — **`[post-v1]`** shared `@ai` |
| `seq_owner_contact_id` | optional string | Trigger user for `ai_on_behalf` — **`[post-v1]`** |
| `ai_invoke_mode` | optional enum | `local` **`[v1]`**; `shared_reply`, `shared_full` **`[post-v1]`** |

**Sync seq rule (E2E only):** `sender_seq` assigned only when `channel=e2e` and `relay_visible=true`. Public relay omits `sender_seq` on the wire. Local-only rows never consume sync seq.

**Not sequenced:** local `@ai`, local-only system rows.

**LLM context:** `content_type=text` plus selected `system` rows only.

### ChatPayload (unified message body — D026)

One schema for disk, relay plaintext (`public_relay`), and AEAD plaintext (`e2e` — E010). Envelope `body.content` holds this object; E2E encrypts its UTF-8 JSON serialization.

**`[v1]` validator** accepts `text` and `system` on inbound relay; rejects unknown types. **`[post-v1]`** enables additional rows in the table below. Enum and columns exist from first schema — do not remove unused types.

```json
{
  "schema_version": 1,
  "content_type": "text",
  "text": "optional human-readable snippet",
  "payload": {}
}
```

| `content_type` | Maturity | `payload` shape (required keys) | UI |
|----------------|----------|----------------------------------|-----|
| `text` | **`[v1]`** | `{}` or `{"format":"plain"}` | Normal bubble; `text` required |
| `system` | **`[v1]`** | `control_type`; optional `detail` | Centered system line |
| `annotation` | **`[post-v1]`** | `annotation_type`, `target_message_id`; optional `value` | Inline on target; badge |
| `contact_card` | **`[post-v1]`** | `contact_id`, `display_name`; optional `relay_user_id`, `avatar_url` | Contact card chrome |
| `crypto_tx` | **`[post-v1]`** | `chain_id`, `asset`, `amount`, `direction`; optional `tx_hash`, `status`, `to_address` | Transaction card |

**`[post-v1]` examples:**

```json
{
  "schema_version": 1,
  "content_type": "annotation",
  "text": "👍",
  "payload": {
    "annotation_type": "reaction",
    "target_message_id": "uuid-of-target",
    "value": { "emoji": "👍" }
  }
}
```

```json
{
  "schema_version": 1,
  "content_type": "contact_card",
  "text": "Alice",
  "payload": {
    "contact_id": "contact:abc",
    "display_name": "Alice",
    "relay_user_id": "relay:user:xyz"
  }
}
```

```json
{
  "schema_version": 1,
  "content_type": "crypto_tx",
  "text": "Sent 0.5 ETH",
  "payload": {
    "chain_id": "eip155:1",
    "asset": "ETH",
    "amount": "0.5",
    "direction": "send",
    "tx_hash": "0x…",
    "status": "confirmed"
  }
}
```

**`[post-v1]` display:** `BuildDisplayRows` merges `annotation` onto `target_message_id`; other types use templates. Orphan targets: standalone row + badge (D043). Cap: **`kMaxAnnotationsPerTarget`** (32, D042). Annotations are separate messages (D005), not target mutations.

**LLM context:** `content_type=text` (+ selected `system`) unless summarized in `text`.

### Durable memory (per thread)

| Artifact | Location | Notes |
|----------|----------|-------|
| `ConversationSummary` | `thread.db` → `memory` table | Text + version; from `ICompactionService` (D040) |
| Future fact rows | Same table (kv) | **`[future]`** |

## On-disk layout (target — D025, D028, D035, D036)

Profile-scoped storage:

```
{data_dir}/profiles/{profile_id}/
  threads/
    profile.db                     # threads catalog + outbox + chat_targets (D017, D035, D047)
    {thread_id}/
      thread.db                     # messages, memory, sync_state — authoritative transcript
```

No `sync/chat_targets.json` — chat-target counters live in `profile.db` (D047).

**Thread exists** iff `{thread_id}/thread.db` is present. `profile.db` `threads` row is a list cache; repaired lazily on sidebar list (D035).

Delete thread = `profile.db` transaction (`DELETE` from `threads` + `outbox` for `thread_id`) then remove `{thread_id}/` directory.

### `thread.db` schema (v1)

```sql
CREATE TABLE messages (
  id TEXT PRIMARY KEY,
  sender_contact_id TEXT NOT NULL,
  content_type TEXT NOT NULL,
  payload TEXT NOT NULL,             -- JSON object (ChatPayload.payload + extended fields)
  text TEXT,
  content_rml TEXT,
  user_payload TEXT,
  timestamp INTEGER NOT NULL,
  relay_visible INTEGER NOT NULL,
  delivery TEXT NOT NULL,
  transport TEXT,
  sender_seq INTEGER,
  session_epoch INTEGER,
  target_message_id TEXT,
  generation TEXT,
  seq_owner_contact_id TEXT,
  ai_invoke_mode TEXT,
  control_type TEXT
);
CREATE INDEX idx_messages_seq ON messages(session_epoch, sender_contact_id, sender_seq)
  WHERE relay_visible = 1;
CREATE INDEX idx_messages_delivery ON messages(delivery) WHERE relay_visible = 1;
CREATE INDEX idx_messages_timestamp ON messages(timestamp DESC);

CREATE TABLE memory (
  key TEXT PRIMARY KEY,              -- e.g. "summary"
  value TEXT NOT NULL                -- JSON
);

CREATE TABLE sync_state (
  peer_contact_id TEXT NOT NULL,
  session_epoch INTEGER NOT NULL,
  state_json TEXT NOT NULL,          -- watermarks, sync_state, user_resolution,
                                     -- integrity_incidents[] (D038, D049)
  PRIMARY KEY (peer_contact_id, session_epoch)
);
```

`PRAGMA user_version` on each `thread.db` and `profile.db` for schema bumps (D016: wipe on mismatch).

### `profile.db` schema (v1)

```sql
CREATE TABLE threads (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL,                -- ai | direct | group
  channel TEXT NOT NULL DEFAULT '',  -- public_relay | e2e; empty for ai/group v1
  title TEXT NOT NULL,
  participant_contact_ids TEXT NOT NULL,  -- JSON array
  preview TEXT,
  updated_at INTEGER NOT NULL,
  unread_count INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_threads_updated ON threads(updated_at DESC);
CREATE INDEX idx_threads_direct ON threads(kind, channel);

CREATE TABLE outbox (
  message_id TEXT PRIMARY KEY,
  thread_id TEXT NOT NULL,
  delivery TEXT NOT NULL             -- pending | failed
);
CREATE INDEX idx_outbox_thread ON outbox(thread_id);

CREATE TABLE chat_targets (
  contact_id TEXT NOT NULL,
  channel TEXT NOT NULL,             -- public_relay | e2e
  session_epoch INTEGER NOT NULL DEFAULT 1,
  next_outgoing_seq INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (contact_id, channel)
);
```

**Scope:** `profile.db` holds the **sidebar list cache** (`threads`), **durable outbox index** (`outbox`), and **chat-target seq state** (`chat_targets`, D047). It does **not** store message-id dedup state (D034).

**Per-thread dedup:** `HasMessageId(thread_id, message_id)` → `SELECT 1 FROM messages WHERE id = ?` on that thread's `thread.db` (`messages.id` is PRIMARY KEY). Relay poll, send retry, and multi-path delivery duplicates are always scoped to one `thread_id` on the envelope. **Clear history** deletes transcript rows (dedup surface wiped); below-floor traffic is excluded by seq (D037), not by a separate message-id tombstone.

Startup durable outbox scan → `SELECT * FROM outbox` (D017), then **reconcile** against `thread.db` (see § Startup reconciliation). Purge `outbox` rows on `DeleteThread`.

### Startup reconciliation (D047)

Run once per profile open after `ListPendingOutbox`:

| Check | Action |
|-------|--------|
| Outbox row, no message in `thread.db` | Delete orphan outbox row; log warning |
| Outbox row, message exists, `delivery=relayed` | Delete stale outbox row |
| Message `delivery=pending`/`failed`, no outbox row | Insert outbox row (repair from authoritative `thread.db`) |
| `chat_targets` row missing for active direct thread | Insert default `(epoch=1, next_outgoing_seq=1)` or derive from max local seq |

Optional dev-only: `PRAGMA integrity_check` on `profile.db` and open `thread.db` files; on failure offer delete-thread recovery.

### Epoch bump transaction (D014, cross-project)

Single coordinated flow when user starts new secure chat or peer reset requires epoch bump:

1. Increment `chat_targets.session_epoch`; reset `next_outgoing_seq = 1` in **`profile.db`** (same txn).
2. Update e2e **`sessions.json`** `session_epoch` + re-derive session key ([e2e-message-crypto](../e2e-message-crypto/DESIGN.md)).
3. Update cached `Thread.session_epoch` in `profile.db` `threads` row.
4. Reset per-peer `sync_state` watermarks for the new epoch in `thread.db`.

No separate JSON sidecar — all durable state in SQLite + crypto session store.

### Catalog consistency (D035)

| Concern | Rule |
|---------|------|
| Authority | `thread.db` exists → thread is real; messages table is source for preview text and last activity time when verifying |
| List cache | `profile.db` `threads` — fast sort/filter; may be stale until verify |
| `ListThreads` | Read catalog; **verify visible slice only** (open `thread.db`, check existence, refresh preview/`updated_at` if needed) |
| Profile open | Once per profile: `readdir` — orphan `thread.db` without catalog row → insert stub `threads` row |
| Orphan catalog row | No `thread.db` on visible verify → delete `threads` + `outbox` rows |
| `AppendMessage` | `thread.db` txn first; then `UPDATE threads` (`updated_at`, `unread_count`); preview refresh deferred to verify (active thread may update eagerly) |
| `ClearMessages` | Keep `threads` row; verify shows empty preview |
| `FindOrCreateDirectThread` | Query `profile.db` `threads` by `(contact_id, channel)` — not directory scan |

### Schema versioning (breaking)

- **`user_version`** on SQLite files; **no in-place migration** from legacy flat JSON, `index.json`, or pre-D035 layouts (D016).
- Dev builds: delete `threads/` on bump.

## Clear / forget semantics (user-facing — D024)

**Clear history** opens a **choice sheet** with two actions (labels illustrative):

| Action | Transcript | LLM window | `memory` table | Thread shell | P2P `history_floor_seq` |
|--------|------------|------------|---------------|--------------|-------------------------|
| **Clear messages** | wipe | empty | keep by default; optional checkbox **Also forget what AI learned** wipes `memory` | keep | set per peer/epoch (E2E only) |
| **Delete conversation** | gone | gone | gone | delete catalog row + dir | n/a |

**Forget what AI learned** (separate menu item): transcript unchanged; wipe `memory` table only.

| Other action | Transcript | Memory | Notes |
|--------------|------------|--------|-------|
| **New chat** (AI) | new empty thread dir | empty | n/a |

P2P clear copy notes peer and relay may retain copies. Clear visible levels do **not** reset outgoing seq or `session_epoch` (D010).

## Routing and modes

| Thread kind | User message | Path |
|-------------|--------------|------|
| AI | any (non-payload) | `AgentSession::SubmitToThread` → store → LLM |
| Direct | normal text | `P2pMessagingService::SendUserMessage` → relay (or direct transport) |
| Direct | `@ai …` | Local assist — see table below |
| Direct | structured payload | local action chips — no relay |

### `@ai` in direct threads (D012)

| Mode | Maturity | Syntax | Peer sees | E2E `sender_seq` | Notes |
|------|----------|--------|-----------|------------------|-------|
| **Local** | **`[v1]`** | `@ai …` | Nothing | No | `relay_visible=false`, `ai_invoke_mode=local` |
| **Shared reply** | **`[post-v1]`** | `@ai+ …` | AI output only | +1 | Prompt not relayed |
| **Shared full** | **`[post-v1]`** | `@ai++ …` | Prompt + AI output | +2 | Stripped prompt on wire |

Aliases **`[post-v1]`:** `@ai share …` → shared reply; `@ai share all …` → shared full.

**`[v1]` local flow:** `SubmitScopedAssist` → persist AI row with `sender_contact_id=ai:assistant`, `relay_visible=false`, no `sender_seq`. Composer placeholder: `Message… or @ai ask assistant`.

**`[post-v1]` shared modes — AI on behalf of trigger user:**

- Trigger user owns **`seq_owner_contact_id`** and **`sender_seq`** on the wire; envelope **`sender_contact_id`** = trigger user (`local:self`).
- Local UI may render `ai_on_behalf` as assistant bubble with “Shared” badge.
- **`generation`:** prompt row (shared full) = `user`; AI reply = `ai_on_behalf`.
- Assign `(message_id, sender_seq)` at first local persist; retry same pair on failure (D010).

**`[post-v1]` flows:**

- **Shared reply:** `SubmitScopedAssist(shared_reply)` → on complete, persist + send one row (`generation=ai_on_behalf`, `relay_visible=true`, +1 seq).
- **Shared full:** persist + send prompt (`generation=user`, seq N) → assist → persist + send reply (`generation=ai_on_behalf`, seq N+1).

**`[post-v1]` UX:** confirm before first shared send; transport badge on shared rows when `[post-v1]` transport UI ships. Placeholder: `Message… · @ai · @ai+ · @ai++`. Requires v6 E2E send pipeline.

## Transport provenance (D051)

**`[v1]`:** Persist `transport` (`local` / `relay` / `direct`) at send/receive in `P2pMessagingService` (and future libp2p layer). E2E vs public **thread shell** styling (`.chat-shell--e2e`) in v2b.

**`[post-v1]`:** Per-message indicator in E2E threads — **Direct** (libp2p), **Relay** (fallback), **Local** (`@ai`, system, unsent). Read `transport` column; do not infer from thread type alone.

## Relay / direct envelope (target extension)

UUID dedup unchanged. **E2E** envelopes add `sender_seq` and `session_epoch`; **public relay** omits them (D045):

```json
{
  "thread_id": "uuid",
  "message_id": "uuid",
  "sender_relay_id": "relay:…",
  "sender_contact_id": "contact:…",
  "sender_seq": 42,
  "session_epoch": 1,
  "body": {
    "content": {
      "schema_version": 1,
      "content_type": "text",
      "text": "Hello",
      "payload": {}
    }
  },
  "timestamp": 1234567890,
  "signature": "…"
}
```

(`sender_seq` / `session_epoch` present on **`e2e`** only; public relay uses the same envelope minus those fields.)

**E2E channel** — same outer envelope; `body` uses nested ciphertext (E009):

```json
"body": {
  "e2e": {
    "payload_b64": "…"
  }
}
```

`payload_b64` decodes to `[version:1][nonce:24][ciphertext+tag]`; AEAD plaintext is UTF-8 JSON of the same `ChatPayload` object (E010).

- **`sender_contact_id`** is required on the wire (D021). Do not infer sender from thread participants (breaks group chat and multi-peer correctness).
- Signature covers `(message_id, sender_seq, session_epoch, thread_id, sender_contact_id, …)` so seq cannot be forged independently of the sender key.

### Send pipeline

1. Serialize `next_outgoing_seq` assignment per chat target in **`profile.db`** (mutex, D047).
2. **E2E:** assign `(message_id, sender_seq)` at first local persist when `relay_visible=true`. **Public:** assign `message_id` only.
3. Persist with `delivery=pending` **before** network I/O.
4. Sign and send; on failure set `delivery=failed` and enqueue durable retry (D017).
5. Retries reuse the **same** ids — E2E: `(message_id, sender_seq)`; public: `message_id` only.

### Durable outbox (D017)

| Source | Behavior |
|--------|----------|
| On startup | `profile.db` `outbox` table + optional per-thread verify (D028) |
| In-memory queue | May batch IO; must not be the sole copy of pending state |
| Registry | On append `relay_visible` pending: insert `outbox`; on relayed: delete row |
| Retry policy | Exponential backoff; max **`kMaxOutboxRetryAttempts`** (D041) per message; user-visible notice on persistent failure; manual retry resets counter |
| Relay dedup | Server must accept duplicate `message_id` on retry (idempotent ingest) |

## P2P sync (E2E only — D045)

E2E direct threads use seq-scoped sync. **Public relay:** poll + local `GetMessagesPage` only — no seq modes.

| Mode | Maturity | Trigger | Behavior |
|------|----------|---------|----------|
| **Tail sync** | **`[v1]`** | Open E2E thread, reconnect, new device | Fetch latest **N** peer-visible messages per sender (default **50**) |
| **Gap repair** | **`[v1]`** | Hole in contiguous tail (`seq N` + `seq N+2+`) | Auto backfill from peer (direct) or relay (D027); not scroll-gated |
| **History backfill** | **`[post-v1]`** | User scrolls to top of loaded transcript | Page `sender_seq` in `(history_floor_seq, loaded_min_seq)`; **25** per page via D027 |

**`[v1]` scroll-up:** `GetMessagesPage` on **local** transcript only — no relay fetch for older history.

**`[post-v1]` history backfill** relay request (when floor set, `min_sender_seq = floor + 1`):

```
min_sender_seq=floor+1, max_sender_seq=loaded_min-1, limit=25, order=desc
```

Ingest: authorized backfill only — seq in `(history_floor_seq, loaded_min_seq)` when user initiated scroll fetch. Below-floor rows: silent discard (D037). UX: scroll hint when `loaded_min_seq > 1`.

### Within-epoch sender contract (E2E only)

For a fixed chat target `(contact_id, channel, session_epoch)`, the **sender** must obey:

| Rule | Behavior |
|------|----------|
| S1 | Assign `sender_seq` only when `relay_visible=true`; strictly monotonic 1, 2, 3, … within the epoch |
| S2 | `next_outgoing_seq` never decreases within an epoch |
| S3 | **Clear visible history** (local UI) does **not** reset seq |
| S4 | Failed send retries the **same** `(message_id, sender_seq)` |
| S5 | Local-only rows (`@ai`, local system) do **not** consume seq |
| S6 | The **only** way to emit `sender_seq = 1` again is a **new `session_epoch`** (D014) |
| S7 | Never emit relay-visible content with `sender_seq < next_outgoing_seq` (no reuse, no rewind) |

Receiver treats E2E sender violations as soft compromised ingest (D013) — pause + user choice (D038), not silent best-effort merge.

### Bootstrap vs gap

- **Bootstrap / tail ingest:** empty per-epoch transcript (or new `session_epoch`) may receive high `sender_seq` without backfilling all prior seq — not a gap alarm (D009).
- **New epoch:** `session_epoch` increases → reset per-epoch watermarks for that peer; `sender_seq = 1` is normal bootstrap, not compromised.
- **Contiguous gap:** local state for this epoch already has seq **N** and receives **N+2+** above `history_floor_seq` → `sync_state=gap`, attempt repair (not yet compromised). Repair requests clamp to **`kMaxGapRepairSeqSpan`** (D041); after **`kMaxGapRepairRounds`** failed cycles → soft compromised (D038).

E2E tail sync is **peer-first** (direct/libp2p); relay tail is fallback when the peer is offline or transport fell back.

### Relay API — thread message fetch (D027)

Authoritative **E2E** backfill when peer is offline or for gap repair. Authenticated as relay user (same identity as send).

**Authorization (required):** Relay MUST verify the authenticated caller is a **participant** of `thread_id` before returning messages. Non-participants receive **403**. Client ingest MUST reject envelopes where `sender_contact_id` ∉ `thread.participant_contact_ids` (except `local:self` for own sends).

**`GET /v1/threads/{thread_id}/messages`**

| Query param | Required | Description |
|-------------|----------|-------------|
| `sender_contact_id` | yes | Peer's contact id whose seq stream to fetch |
| `session_epoch` | yes | Epoch scope |
| `min_sender_seq` | no | Inclusive lower bound (gap repair) |
| `max_sender_seq` | no | Inclusive upper bound (gap repair) |
| `limit` | no | Default **50**, max **100** |
| `order` | no | `asc` (default) or `desc` |

**Sync mode usage** (when `history_floor_seq` is set, use `min_sender_seq = floor + 1` on all modes):

| Mode | Typical request |
|------|-----------------|
| Tail sync | `min_sender_seq=floor+1`, `order=desc`, `limit=50` |
| Gap repair | `min_sender_seq=max(N+1, floor+1)`, `max_sender_seq=M`, `order=asc` |
| History backfill **`[post-v1]`** | `min_sender_seq=floor+1`, `max_sender_seq=loaded_min-1`, `limit=25`, `order=desc` |

Discard any below-floor rows in relay responses without compromising (D037).

**Response 200:**

```json
{
  "thread_id": "uuid",
  "session_epoch": 1,
  "sender_contact_id": "contact:…",
  "messages": [ /* RelayEnvelope[] */ ],
  "has_more": true,
  "cursor": {
    "next_min_sender_seq": 10,
    "next_max_sender_seq": null
  }
}
```

- Each element is a full signed `RelayEnvelope` (client verifies signature; **E2E** runs D013 ingest).
- **`POST /v1/messages`** (or existing send): idempotent on `message_id` — duplicate POST returns 200 with same id (D017). Reject body > `kMaxRelayEnvelopeJsonBytes` (D029).
- Inbox **poll** may remain for notifications; clients must not rely on poll alone for seq-complete history. Max **100** messages per poll response (D029/D032).

MCP bridge: expose equivalent `relay_fetch_thread_messages` tool with same parameters for promoted-MCP path.

### Reorder buffer (D020) — E2E only

Implemented via **`ReplayWindow`** helper in `base/crypto` ([e2e-message-crypto](../e2e-message-crypto/DESIGN.md)); **D013 classifier in the feature layer is authoritative** — `ReplayWindow` holds out-of-order slots only; it does not persist or override compromise policy.

During gap repair or multi-path delivery (direct + relay), E2E messages may arrive out of order:

- **`kReorderWindow = 32`** — hold inbound messages with `sender_seq` in `(contiguous_peer_seq, contiguous_peer_seq + kReorderWindow]` before declaring `gap`.
- Messages above the window without filling the hole → `sync_state=gap`, trigger repair.
- After repair, flush buffer in seq order before updating `contiguous_peer_seq`.

### Display ordering (D019)

UI transcript sort (stable):

**E2E (`relay_visible`):** `(session_epoch, sender_contact_id, sender_seq)` ascending.

**Public relay (`relay_visible`):** `timestamp` ascending; tie-break `message_id`.

**Local-only rows** (`relay_visible=false`): interleaved by `timestamp` among themselves.

Gap repair may reorder E2E rows visually when missing rows arrive; scroll position should anchor on read cursor, not raw array index.

### Receive pipeline

Ordered steps — do not reorder in implementation (D022, D033):

0. **Envelope size** — reject if serialized JSON > `kMaxRelayEnvelopeJsonBytes` (D029).
1. **Per-thread UUID dedup** — `HasMessageId(envelope.thread_id, envelope.message_id)`; benign duplicate → stop (D034).
2. **Verify Ed25519 signature** on outer envelope (classical; see e2e-message-crypto).
3. **Thread lookup** — reject unknown `thread_id`; **`sender_contact_id` must be a thread participant** (or `local:self` for reflected sends).
4. **Channel branch:**
   - **`public_relay`:** parse `ChatPayload` (steps 5–6 below) → persist if UUID dedup passes. No seq classifier.
   - **`e2e`:** epoch check → AEAD decrypt → parse → history floor (D037) → **D013 classifier** (step 8) → persist.
5. **Plaintext size** — decrypted UTF-8 JSON ≤ `kMaxE2ePlaintextBytes`; public `body.content` ≤ `kMaxChatPayloadJsonBytes`.
6. **Parse & validate `ChatPayload`** — **`[v1]`** types `text`, `system` on inbound relay; **strip/ignore wire `content_rml`** (D030).
7. **History floor (E2E, D037)** — if `sender_seq ≤ history_floor_seq[peer][epoch]`, silent discard.
8. **D013 ingest classifier (E2E only)** — normal · gap · soft compromised · hard reject; `ReplayWindow` before gap declaration.
9. **Persist** — append `ThreadMessage`, update watermarks (E2E), set `transport` from ingress path.

Validate `thread_id` and `message_id` as UUID before filesystem / DB use.

### Public relay ingest (D045)

After signature verify and participant check: accept if UUID dedup passes and `ChatPayload` validates. Order by `timestamp` for display. No `sync_state`, gap repair, or compromise UX on public channel.

### Ingest classification (E2E only — normal · gap · soft compromised · hard reject)

After crypto/size checks; below-floor already discarded in step 7. **`[v1]`:** always strict — no relaxed override (D046). **`[post-v1]`:** optional relaxed ingest — see § Integrity recovery.

**Normal (accept):**

1. **Benign duplicate** — same `(message_id, sender_seq, session_epoch)` → ignore.
2. **Epoch advance** — `session_epoch` increases → reset per-epoch watermarks; accept as fresh stream (see § Peer reset).
3. **Contiguous tail** — `sender_seq == contiguous_peer_seq + 1` and `sender_seq > history_floor_seq[peer][epoch]`.
4. **Tail bootstrap** — per-epoch transcript empty; ingest tail batch without requiring seq 1..N first (only seq **> floor** when floor is set).

**Gap (repair allowed; not compromised until repair fails):**

- `sender_seq > contiguous_peer_seq + 1` and `sender_seq > history_floor_seq[peer][epoch]` → request missing range; on success, reclassify as normal.
- If repair returns seq conflicts or impossible ranges → **soft compromised** (D038 choice sheet). Below-floor rows in a response are silently discarded (D037), not a compromise trigger.

**Soft compromised (pause + user choice — D038):**

| Condition | Why |
|-----------|-----|
| Same `(peer, epoch, sender_seq)` + **different** `message_id` | Seq conflict (D011) |
| `sender_seq < contiguous_peer_seq` and not benign duplicate | Rewind within epoch |
| `sender_seq = 1` in an **established** epoch where `contiguous_peer_seq > 0` | Sender reset without epoch bump |
| Gap repair exhausted or returns violating messages | Repair failed |

On soft compromised: **pause ingest and outbound**, set `sync_state=compromised`, append **integrity incident**, show choice sheet. Do not persist the triggering inbound row until user resolves (except record incident metadata). Recovery: see § Integrity recovery.

**Hard reject:**

| Condition | Why |
|-----------|-----|
| `session_epoch` **decreases** | Illegal rollback |
| Invalid signature / AEAD decrypt failure / wrong thread / envelope epoch mismatch | Wire or crypto invalid |
| `sender_contact_id` not a participant | Authorization |

Reject message permanently. Pause ingest/outbound if not already paused; show incident with **Pause only** or recommended recovery.

### Integrity recovery (D038)

**E2E only** for seq compromise UX. Public channel has no seq classifier (D045).

#### `[v1]` recovery

On soft compromised: pause → choice sheet (what, causes, risk) → user **must** pick:

| Option | Action |
|--------|--------|
| **Start new secure chat** | `rotate_psk`, epoch bump transaction |
| **Pause only** | remain paused |

No **continue anyway** in v1 (D046).

**E2E new secure chat flow** (`rotate_psk`):

Local state machine: `ok` → `gap` → `compromised` → `awaiting_new_psk` → `ok`.

```
Initiating side                               Innocent peer
     |                                              |
     | 1. User confirms new secure chat             | 1. May see peer pause banner
     | 2. Exchange new PSK OOB                      | 2. Accept higher session_epoch
     | 3. Epoch bump transaction (§ above)          |    on first message in new epoch
     | 4. Resume at seq 1+                          | 3. Fresh watermarks for new epoch
```

- Innocent peer accepts **strictly higher** `session_epoch` (D014).
- **No `epoch_start` system row** — first user message may use `sender_seq=1`.
- Old epoch keys: decrypt historical ciphertext only; no new ingest on old epoch after rotation.

#### `[post-v1]` relaxed ingest / continue anyway (D046 extension)

If product enables informed override after strict detection, extend `sync_state.state_json`:

| Field | Values |
|-------|--------|
| `ingest_policy` | `strict` \| `relaxed` |
| `user_resolution` | adds `continue_anyway` |
| `trust_degraded` | bool — persistent banner |

Choice sheet adds secondary **Continue with current keys** (E2E) after disclosure. Hard crypto failures: no override.

**Relaxed rules** (`ingest_policy=relaxed`, `user_resolution=continue_anyway`):

| Situation | Rule |
|-----------|------|
| Seq conflict | Keep first-seen `(peer, epoch, sender_seq)`; discard conflicting inbound |
| Rewind / non-contiguous | Accept inbound; advance `contiguous_peer_seq` only on strict increase; gaps in UI |
| Outbound | Re-enable sends; peer may still be strict — local override ≠ protocol agreement |

Return to strict via new secure chat, delete thread, or explicit reset.

### Clear history and seq (D037)

| Party | Behavior |
|-------|----------|
| **Sender** | `next_outgoing_seq` and `session_epoch` on chat target unchanged; next live send uses next seq as usual (e.g. 101) |
| **Receiver** | `DELETE FROM messages`; set `history_floor_seq[peer][epoch]` to max contiguous seq seen at clear time; reset `loaded_min`/`loaded_max`/`contiguous_peer_seq` watermarks for display (or derive from empty transcript) |
| **Below floor** | `sender_seq ≤ floor` in same epoch → **silent discard** on all paths (poll, direct, tail, gap). No persist, backfill, show, or unread bump. |
| **Above floor** | Normal D013 ingest — tail, gap repair; **`[post-v1]`** authorized history backfill in `(floor, loaded_min)` |

The sender does not need a signal that the peer cleared locally; honest senders continue forward. Per-thread dedup (D034) is wiped with the transcript; seq floor — not a message-id registry — defines the sync boundary after clear. Full restart requires **epoch bump** (D014), not clear.

### Peer reset / new device (fresh stream)

When a peer wipes local state, installs on a new device without backup, or explicitly starts over:

1. **Bump `session_epoch`** via epoch bump transaction (D014).
2. Reset `next_outgoing_seq = 1` for the new epoch.
3. **No `epoch_start` system message** — first relay-visible user content may use `sender_seq=1`.
4. **Receiver** on unseen higher epoch: fresh per-epoch watermarks; `sender_seq=1` is normal bootstrap.

**Restored backup** (same `profile.db` + crypto sessions): not a reset — continue same epoch and seq.

Sending `sender_seq=1` without bumping epoch in an established epoch is **soft compromised** (D038 choice sheet; recommended path is epoch bump).

Benign duplicate delivery (same `message_id` + same `sender_seq`) is ignored via UUID dedup.

## Resource & trust bounds (D029–D033)

Canonical limits in [DECISIONS.md](DECISIONS.md) D029. Summary:

| Area | Policy |
|------|--------|
| Compose / send | Reject empty and > 64 KiB `text`; validate `ChatPayload` before send |
| Wire | Max 256 KiB envelope JSON; **no remote `content_rml`** (D030) |
| Storage | Size checks on insert; LRU of open `thread.db` (max 16); `user_payload` ≤ 64 KiB (D029) |
| UI | `GetMessagesPage` default 100 rows (D031) |
| Agent context | `GetMessagesForContext` tail + summary — no full-thread load (D039) |
| Poll | Min **2 s** interval while foreground (D032); max 100 messages per batch |
| Outbox | `profile.db`-backed; max 500 pending retry items; **12** attempts per message (D041) |
| Gap repair | Max **5** rounds, **500** seq span per fetch (D041) |
| Integrity incidents | Max **10** per `(peer, epoch)` ring buffer (D049) |
| Compaction | Trigger at **20** turns; summary ≤ **8 KiB** (D040) |

**Local assistant `content_rml`** is trusted-local only (AI parser output), max 256 KiB on disk.

**SQLite:** WAL + per-DB writer mutex; passive checkpoint after clear (D044); optional passive checkpoint every N appends or on background idle.

Non-chat limits (LLM HTTP responses, `contacts.json`, `identity.json`) live in [platform-safety-limits](../platform-safety-limits/).

## Store interface (target)

`SqliteThreadStore` implements `IThreadStore`; lazy-open `thread.db` per active thread. Sidebar list reads `profile.db` `threads`; visible-row verify opens only the viewport slice of `thread.db` files (D035).

Extend `IThreadStore`:

```cpp
// Illustrative — names may change during implementation
virtual Roe<void> ClearMessages(const std::string& thread_id) = 0;
virtual Roe<void> SetThreadMemory(const std::string& thread_id, ConversationSummary summary) = 0;
virtual Roe<std::optional<ConversationSummary>> GetThreadMemory(const std::string& thread_id) const = 0;
virtual Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id, ThreadChannel channel) = 0;

// v6 — seq-range reads (natural SQLite index use; avoid loading full transcript)
virtual Roe<std::vector<ThreadMessage>> GetMessagesBySeqRange(
    const std::string& thread_id, uint32_t session_epoch,
    const std::string& sender_contact_id,
    std::optional<uint64_t> min_seq, std::optional<uint64_t> max_seq,
    size_t limit, bool ascending) const = 0;

// D017 — startup outbox without scanning all thread.db files
virtual Roe<std::vector<std::pair<std::string, std::string>>> ListPendingOutbox() const = 0;

// D034 — per-thread ingest dedup (replaces profile-global HasMessageId(message_id))
virtual Roe<bool> HasMessageId(const std::string& thread_id, const std::string& message_id) const = 0;

// D031 — UI transcript window (newest-first or oldest-first via parameter)
virtual Roe<std::vector<ThreadMessage>> GetMessagesPage(
    const std::string& thread_id,
    std::optional<int64_t> before_timestamp,
    size_t limit = 100) const = 0;

// D039 — agent / LLM context (tail + optional summary; no full-thread scan)
virtual Roe<std::vector<ThreadMessage>> GetMessagesForContext(
    const std::string& thread_id, const ContextBudget& budget) const = 0;
```

Send path: reject compose text and serialized payload over D029 limits before `AppendMessage`.

Keep `DeleteThread`, `AppendMessage`, `UpdateMessage`. Change `HasMessageId` to **`HasMessageId(thread_id, message_id)`** — per-thread `messages.id` lookup (D034); drop profile-global dedup from `JsonThreadStore` cutover.

`GetMessages(thread_id)` — **tests and export only**; feature code uses `GetMessagesPage` (UI) or `GetMessagesForContext` (agent).

### SQLite operations (D044)

- WAL mode; one writer mutex per `thread.db` and `profile.db`.
- `ClearMessages` → `DELETE FROM messages` then `wal_checkpoint(PASSIVE)`.
- No auto-VACUUM in v1.

## UI (target)

### Sidebar `[v1]`

- **Flat list** sorted by `updated_at`; direct rows show **Public** / **Private** channel badge (D023). Same contact may appear twice.

### Sidebar `[post-v1]` (optional)

- Collapsible sections **AI**, **Public**, **Private** — presentation-only alternative to flat list + badge.

### Thread chrome & messages

- **E2E vs public shell** `[v1]` — `.chat-shell--e2e` / `.chat-shell--public` ([UI_DESIGN_SYSTEM.md](../../docs/UI_DESIGN_SYSTEM.md)).
- **Message row** `[v1]` — delivery state; text bubbles (extend templates per `content_type` in `[post-v1]`).
- **Transport badge** `[post-v1]` — per-message indicator; reads `transport` column (§ Transport provenance).
- **Windowed transcript (D031)** `[v1]` — loaded page via `GetMessagesPage`; scroll-up local pages; `[post-v1]` may trigger relay history backfill at top.
- **Sidebar list (D035)** `[v1]` — `ListThreads` + visible-row verify/repair.
- **Gap banner** `[v1]` (E2E) — `sync_state=gap`; tap to retry.
- **Integrity banner** `[v1]` (E2E) — `sync_state=compromised`; choice sheet per § Integrity recovery.
- **`@ai`** `[v1]` local; `[post-v1]` shared modes — § `@ai` in direct threads.
- **Clear history (D024)** `[v1]` — clear messages (+ optional forget-AI checkbox) / delete conversation.
- **Forget AI memory** `[v1]` — separate action (`memory` table only).

## Non-goals

Not planned (distinct from **`[post-v1]`** items above, which *are* specified):

- **Multi-device concurrent send** on one identity (D015 — single active device)
- **Group direct seq / ingest** — D013 applies to **1:1 direct** only; `kind=group` deferred
- **Legacy on-disk migration** from pre-D028 JSON (D016)
- Full-text search UI (SQLite FTS **`[future]`**)
- Group E2E
- Retraction / “unsend” on relay
- SQLCipher / transcript encryption at rest (D048 — wire-only E2E confidentiality in v1)

## Success criteria

### `[v1]`

- [ ] All AI sidebar threads persist via `IThreadStore` (no orphan `Conversation`-only path).
- [ ] Clear history / forget memory / delete conversation per D024.
- [ ] Same contact: separate public and E2E threads; channel badge in sidebar.
- [ ] Message IDs stable; relay dedup on both channels.
- [ ] **E2E:** `sender_seq`, tail + gap sync, integrity UX (rotate or pause only).
- [ ] **Public:** UUID dedup + timestamp order; no seq classifier.
- [ ] Durable outbox + `chat_targets` in `profile.db` + reconciliation (D047).
- [ ] Clear history floor (D037); epoch bump transaction (D014).
- [ ] Local `@ai` only; ChatPayload `text` + `system`; D029–D032 bounds.
- [ ] `GetMessagesForContext` hot path; async compaction (D040).
- [ ] Relay fetch + ingest participant authorization (D027).

### `[post-v1]` / mature (enable when phased)

- [ ] Rich ChatPayload types render; annotation cap + orphan UX (D042–D043).
- [ ] Shared `@ai+` / `@ai++` with correct seq on trigger user stream.
- [ ] Scroll-driven history backfill via D027.
- [ ] Per-message transport badge when libp2p direct exists.
- [ ] Optional relaxed ingest / continue anyway (if product enables).
- [ ] Optional sidebar grouped sections.
