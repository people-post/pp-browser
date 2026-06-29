# Design — desired end state

## Principles

1. **One transcript model** — UI, disk, and LLM context derive from the same message store (`ThreadMessage` / future extensions), not parallel in-memory shapes.
2. **Local source of truth** — write locally before send; relay rejections do not erase history ([P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md)).
3. **Three storage layers** — distinguish what the user sees, what fits in the LLM window, and what the agent remembers long-term.
4. **Channel isolation** — public (relay) and E2E conversations with the same contact are different threads with different memory boundaries.
5. **Stable IDs everywhere** — messages, threads, and annotation targets use UUIDs for dedup, sync, and reactions.
6. **Sender sequence for completeness** — peer-visible direct messages carry a per-sender monotonic `sender_seq` (in addition to UUID) so receivers detect gaps in the live tail; UUID remains the only message identity. Only **`relay_visible`** content consumes sync seq (see `@ai` modes below).
7. **Strict normal-or-compromised ingest (direct chat)** — in **all direct threads** (`public_relay` and `e2e`), the receiver classifies inbound traffic with D013: **normal**, **gap**, **soft compromised** (seq integrity), or **hard reject** (wire/crypto). Soft failures **pause** ingest and outbound by default and show a **user choice sheet** (D038); recommended recovery differs by channel (E2E → new PSK + epoch; public → delete thread). User may **continue anyway** under relaxed ingest after disclosure. Hard failures cannot be overridden. Violations are never silently merged under default strict policy.
8. **Durable outbox** — `relay_visible` rows with `delivery=pending` or `failed` survive app restart; retries reuse the same `(message_id, sender_seq)` (D017).
9. **Storage abstraction** — `IThreadStore` stays the seam; **`SqliteThreadStore`** per-thread `thread.db` + `threads/profile.db` (`threads` catalog + `outbox` index, D028, D035, D036) from v2a. No `index.json` or other JSON thread files.

## Assumptions (v1)

| Assumption | Implication |
|------------|-------------|
| **Single active sender per identity** (D015) | One client per profile may send on a chat target at a time. Two devices with the same identity and PSK without coordination will emit conflicting `sender_seq` → compromised (D011). Document in UX; multi-device seq coordination is out of scope for v1. |
| **No legacy thread migration** (D016) | Legacy flat `threads/{id}.json` is not upgraded. Schema bumps may require wiping `{data_dir}/profiles/{id}/threads/` (acceptable — no production users yet). |

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

Persist in thread metadata or a small sidecar keyed by `(contact_id, channel)`.

### Per-peer sync state (per thread, scoped by `session_epoch`)

Sync watermarks are keyed by **`(peer, session_epoch)`**. A new epoch starts a fresh stream; old-epoch state is retained for history display but not used for gap logic on the new epoch.

| Field | Notes |
|-------|-------|
| `contiguous_peer_seq[peer][epoch]` | Highest seq received with no holes in the active tail for this epoch |
| `loaded_min_seq[peer][epoch]`, `loaded_max_seq[peer][epoch]` | Oldest/newest peer seq present in local transcript for this epoch |
| `history_floor_seq[peer][epoch]` | Set on **clear visible history** — seq at or below this in the same epoch is **excluded from sync** (D037): silent discard, not compromised |
| `sync_state` | `ok` \| `gap` \| `compromised` — `compromised` means paused pending user resolution (D038) |
| `ingest_policy` | `strict` (default) \| `relaxed` — classifier mode after user chooses continue anyway (D038) |
| `user_resolution` | `null` \| `rotate_psk` \| `reset_thread` \| `continue_anyway` \| `pause_only` |
| `trust_degraded` | bool — persistent after `continue_anyway`; show integrity badge in thread chrome |
| `integrity_incidents[]` | `{ kind, detected_at, detail }` — append-only audit log per `(peer, epoch)` |

### ThreadMessage

| Field | Type | Notes |
|-------|------|-------|
| `id` | UUID | Client-generated; dedup on ingest |
| `thread_id` | UUID | |
| `content_type` | enum | **New.** `text`, `annotation`, `contact_card`, `crypto_tx`, `system` — see § ChatPayload |
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
| `control_type` | optional string | For `content_type=system`, e.g. `epoch_start` (D014) |
| `sender_seq` | optional uint64 | Sync seq on `relay_visible` content only |
| `session_epoch` | optional uint32 | Must match envelope |
| `generation` | optional enum | `user` \| `ai_on_behalf` |
| `seq_owner_contact_id` | optional string | Trigger user for `ai_on_behalf` rows |
| `ai_invoke_mode` | optional enum | `local` \| `shared_reply` \| `shared_full` |

**Sync seq rule:** `sender_seq` assigned only when `relay_visible=true`. Local-only rows never consume sync seq.

**Not sequenced:** local `@ai`, local-only annotations (`relay_visible=false`), local-only system rows.

**LLM context:** `content_type=text` plus selected `system` rows only — not annotations, cards, or txs unless summarized in `text`.

### ChatPayload (unified message body — D026)

One schema for disk, relay plaintext (`public_relay`), and AEAD plaintext (`e2e` — E010). Envelope `body.content` holds this object; E2E encrypts its UTF-8 JSON serialization.

```json
{
  "schema_version": 1,
  "content_type": "text",
  "text": "optional human-readable snippet",
  "payload": {}
}
```

| `content_type` | `payload` shape (required keys) | UI |
|----------------|----------------------------------|-----|
| `text` | `{}` or `{"format":"plain"}` | Normal bubble; `text` required |
| `annotation` | `annotation_type`, `target_message_id`; optional `value` (emoji, edit body, etc.) | Inline on target or thread row; badge |
| `contact_card` | `contact_id`, `display_name`; optional `relay_user_id`, `avatar_url` | Contact card chrome; share/add actions |
| `crypto_tx` | `chain_id`, `asset`, `amount`, `direction` (`send`\|`request`); optional `tx_hash`, `status`, `to_address` | Transaction card; explorer link when hash present |
| `system` | `control_type` (e.g. `epoch_start`); optional `detail` | Centered system line |

**Annotation example** (relayed like — consumes seq when `relay_visible=true`):

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

**Contact card example:**

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

**Crypto transaction example:**

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

`BuildDisplayRows` merges `annotation` rows onto `target_message_id` for rendering; other types render as standalone rows with type-specific templates.

### Durable memory (per thread)

| Artifact | Location | Notes |
|----------|----------|-------|
| `ConversationSummary` | `thread.db` → `memory` table | Text + version; from `ICompactionService` |
| Future fact rows | Same table (kv) | Deferred |

## On-disk layout (target — D025, D028, D035, D036)

Profile-scoped storage:

```
{data_dir}/profiles/{profile_id}/
  threads/
    profile.db                     # threads catalog (sidebar cache) + outbox index (D017, D035)
    {thread_id}/
      thread.db                     # messages, memory, sync_state — authoritative transcript
  sync/
    chat_targets.json               # next_outgoing_seq, session_epoch per (contact_id, channel)
```

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

CREATE TABLE memory (
  key TEXT PRIMARY KEY,              -- e.g. "summary"
  value TEXT NOT NULL                -- JSON
);

CREATE TABLE sync_state (
  peer_contact_id TEXT NOT NULL,
  session_epoch INTEGER NOT NULL,
  state_json TEXT NOT NULL,          -- watermarks, sync_state, ingest_policy, trust_degraded,
                                     -- user_resolution, integrity_incidents[] (D038)
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
```

**Scope:** `profile.db` holds the **sidebar list cache** (`threads`) and **durable outbox index** (`outbox`). It does **not** store message-id dedup state (D034).

**Per-thread dedup:** `HasMessageId(thread_id, message_id)` → `SELECT 1 FROM messages WHERE id = ?` on that thread's `thread.db` (`messages.id` is PRIMARY KEY). Relay poll, send retry, and multi-path delivery duplicates are always scoped to one `thread_id` on the envelope. **Clear history** deletes transcript rows (dedup surface wiped); below-floor traffic is excluded by seq (D037), not by a separate message-id tombstone.

Startup durable outbox scan → `SELECT * FROM outbox` (D017), not full table scan of every `thread.db`. Purge `outbox` rows on `DeleteThread`.

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

**Clear history** opens a **choice sheet** with three levels (labels illustrative):

| Level | Transcript | LLM window | `memory` table | Thread shell | P2P `history_floor_seq` |
|-------|------------|------------|---------------|--------------|-------------------------|
| **Clear messages** | wipe | empty | keep | keep | set per peer/epoch |
| **Clear messages & AI memory** | wipe | empty | wipe | keep | set per peer/epoch |
| **Delete conversation** | gone | gone | gone | delete catalog row + dir | n/a |

**Forget what AI learned** (separate menu item): transcript unchanged; wipe `memory` table only.

| Other action | Transcript | Memory | Notes |
|--------------|------------|--------|-------|
| **New chat** (AI) | new empty thread dir | empty | n/a |

P2P clear levels include copy that peer and relay may retain copies. Clear visible levels do **not** reset outgoing seq or `session_epoch` (D010).

## Routing and modes

| Thread kind | User message | Path |
|-------------|--------------|------|
| AI | any (non-payload) | `AgentSession::SubmitToThread` → store → LLM |
| Direct | normal text | `P2pMessagingService::SendUserMessage` → relay (or direct transport) |
| Direct | `@ai …` | Local assist — see table below |
| Direct | structured payload | local action chips — no relay |

### `@ai` in direct threads (three modes)

Composer syntax (local is default for privacy):

| Mode | Syntax | Peer sees | Sync `sender_seq` | Notes |
|------|--------|-----------|-------------------|-------|
| **Local** | `@ai …` | Nothing | No | Private copilot; `relay_visible=false`, `ai_invoke_mode=local` |
| **Shared reply** | `@ai+ …` | AI output only | +1 (reply row) | Prompt not relayed; AI speaks on behalf of trigger user |
| **Shared full** | `@ai++ …` | Prompt + AI output | +2 (prompt, then reply) | Prompt body is stripped text (no `@ai++` prefix on wire) |

Long-form aliases optional in parser: `@ai share …` → shared reply; `@ai share all …` → shared full.

**Shared modes — AI on behalf of trigger user:**

- Trigger user owns **`seq_owner_contact_id`** and the **`sender_seq`** stream on the wire.
- Envelope **`sender_contact_id`** = trigger user (`local:self`) for gap detection on the peer’s view of your stream.
- Local UI may still render `ai_on_behalf` rows as assistant bubbles with “Shared” / “AI assisted” badge.
- **`generation`:** prompt row (shared full only) = `user`; AI reply = `ai_on_behalf`.
- Assign `(message_id, sender_seq)` at first local persist; relay after store; failed send retries same pair (D010).

**Local mode flow:** `SubmitScopedAssist` → persist AI row with `sender_contact_id=ai:assistant`, `relay_visible=false`, no `sender_seq`.

**Shared reply flow:** `SubmitScopedAssist(shared_reply)` → on complete, persist + send one row (`generation=ai_on_behalf`, `relay_visible=true`, +1 seq).

**Shared full flow:** persist + send prompt row (`generation=user`, stripped text, seq N) → `SubmitScopedAssist(shared_full)` → on complete, persist + send reply row (`generation=ai_on_behalf`, seq N+1).

**UX:** confirm before first shared send in a thread (copy differs for `@ai+` vs `@ai++`); E2E transport badge on shared rows. Placeholder: `Message… · @ai · @ai+ · @ai++`.

## Transport provenance (private / E2E UI)

In E2E threads, show per-message indicator:

- **Direct** — libp2p (or future direct path)
- **Relay** — fell back to relay (privacy-relevant)
- **Local** — local `@ai`, system, unsent draft

Set `transport` at send/receive in `P2pMessagingService` (and future libp2p layer), not inferred in UI.

## Relay / direct envelope (target extension)

UUID dedup unchanged. Add fields for ordering, session scope, and signed integrity:

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

1. Serialize `next_outgoing_seq` assignment per chat target (mutex or store-level increment).
2. Assign `(message_id, sender_seq)` at first local persist (`relay_visible=true` only).
3. Persist with `delivery=pending` **before** network I/O.
4. Sign and send; on failure set `delivery=failed` and enqueue durable retry (D017).
5. Retries reuse the **same** `(message_id, sender_seq)` — only `delivery` and transport may change.

### Durable outbox (D017)

| Source | Behavior |
|--------|----------|
| On startup | `profile.db` `outbox` table + optional per-thread verify (D028) |
| In-memory queue | May batch IO; must not be the sole copy of pending state |
| Registry | On append `relay_visible` pending: insert `outbox`; on relayed: delete row |
| Retry policy | Exponential backoff; cap attempts per message; user-visible notice on persistent failure |
| Relay dedup | Server must accept duplicate `message_id` on retry (idempotent ingest) |

## P2P sync (direct / E2E)

Three **separate** sync modes — do not conflate lazy history with live gap repair:

| Mode | Trigger | Behavior |
|------|---------|----------|
| **Tail sync** | Open thread, reconnect, new device | Fetch latest **N** peer-visible messages per sender (default **50**) |
| **Gap repair** | Hole in contiguous tail (`seq N` + `seq N+2+`) | Automatic backfill from peer (direct) or relay fallback; **not** gated on scroll |
| **History backfill** | User scrolls to top of loaded transcript | Page older messages with `sender_seq` in `(history_floor_seq, loaded_min_seq)`; page size **25** (D037) |

### Within-epoch sender contract

For a fixed chat target `(contact_id, channel, session_epoch)`, the **sender** must obey:

| Rule | Behavior |
|------|----------|
| S1 | Assign `sender_seq` only when `relay_visible=true`; strictly monotonic 1, 2, 3, … within the epoch |
| S2 | `next_outgoing_seq` never decreases within an epoch |
| S3 | **Clear visible history** (local UI) does **not** reset seq |
| S4 | Failed send retries the **same** `(message_id, sender_seq)` |
| S5 | Local-only rows (`@ai`, annotations, local system) do **not** consume seq |
| S6 | The **only** way to emit `sender_seq = 1` again is a **new `session_epoch`** (D014) |
| S7 | Never emit relay-visible content with `sender_seq < next_outgoing_seq` (no reuse, no rewind) |

Receiver treats sender violations as soft compromised ingest (D013) by default — pause + user choice (D038), not silent best-effort merge.

### Bootstrap vs gap

- **Bootstrap / tail ingest:** empty per-epoch transcript (or new `session_epoch`) may receive high `sender_seq` without backfilling all prior seq — not a gap alarm (D009).
- **New epoch:** `session_epoch` increases → reset per-epoch watermarks for that peer; `sender_seq = 1` is normal bootstrap, not compromised.
- **Contiguous gap:** local state for this epoch already has seq **N** and receives **N+2+** above `history_floor_seq` → `sync_state=gap`, attempt repair (not yet compromised).

E2E tail sync is **peer-first** (direct/libp2p); relay tail is fallback when the peer is offline or transport fell back.

### Relay API — thread message fetch (D027)

Authoritative backfill when peer is offline or for gap/history sync. Authenticated as relay user (same identity as send).

**`GET /v1/threads/{thread_id}/messages`**

| Query param | Required | Description |
|-------------|----------|-------------|
| `sender_contact_id` | yes | Peer's contact id whose seq stream to fetch |
| `session_epoch` | yes | Epoch scope |
| `min_sender_seq` | no | Inclusive lower bound (gap repair) |
| `max_sender_seq` | no | Inclusive upper bound (history backfill) |
| `limit` | no | Default **50**, max **100** |
| `order` | no | `asc` (default) or `desc` |

**Sync mode usage** (when `history_floor_seq` is set, use `min_sender_seq = floor + 1` on all modes):

| Mode | Typical request |
|------|-----------------|
| Tail sync | `min_sender_seq=floor+1`, `order=desc`, `limit=50` |
| Gap repair | `min_sender_seq=max(N+1, floor+1)`, `max_sender_seq=M`, `order=asc` |
| History backfill | `min_sender_seq=floor+1`, `max_sender_seq=loaded_min-1`, `limit=25`, `order=desc` |

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

- Each element is a full signed `RelayEnvelope` (client verifies signature, runs D013 ingest).
- **`POST /v1/messages`** (or existing send): idempotent on `message_id` — duplicate POST returns 200 with same id (D017). Reject body > `kMaxRelayEnvelopeJsonBytes` (D029).
- Inbox **poll** may remain for notifications; clients must not rely on poll alone for seq-complete history. Max **100** messages per poll response (D029/D032).

MCP bridge: expose equivalent `relay_fetch_thread_messages` tool with same parameters for promoted-MCP path.

### Reorder buffer (D020)

During gap repair or multi-path delivery (direct + relay), messages may arrive out of order.

- **`kReorderWindow = 32`** — hold inbound messages with `sender_seq` in `(contiguous_peer_seq, contiguous_peer_seq + kReorderWindow]` before declaring `gap`.
- Messages above the window without filling the hole → `sync_state=gap`, trigger repair.
- After repair, flush buffer in seq order before updating `contiguous_peer_seq`.

### Display ordering (D019)

UI transcript sort (stable):

1. **`relay_visible` content:** `(session_epoch, sender_contact_id, sender_seq)` ascending.
2. **Local-only rows** (`relay_visible=false`): interleaved by `timestamp` among themselves; never sorted between another sender’s sequenced rows.
3. **Tie-break:** `timestamp`, then `message_id`.

Gap repair may reorder visually when missing rows arrive; scroll position should anchor on read cursor, not raw array index.

### Receive pipeline (direct / E2E)

Ordered steps — do not reorder in implementation (D022, D033):

0. **Envelope size** — reject if serialized JSON > `kMaxRelayEnvelopeJsonBytes` (D029).
1. **Per-thread UUID dedup** — `HasMessageId(envelope.thread_id, envelope.message_id)`; benign duplicate → stop (D034).
2. **Verify Ed25519 signature** on outer envelope (classical; see e2e-message-crypto).
3. **Thread / channel / epoch** — reject wrong `thread_id` or epoch mismatch before decrypt.
4. **Decrypt AEAD** (`e2e` only) — canonical AAD must match envelope fields; failure → reject (no persist).
5. **Plaintext size** — decrypted UTF-8 JSON ≤ `kMaxE2ePlaintextBytes`; public `body.content` ≤ `kMaxChatPayloadJsonBytes`.
6. **Parse & validate `ChatPayload`** — schema version + `content_type`; **strip ignore wire `content_rml`** (D030).
7. **History floor (D037)** — if `sender_seq ≤ history_floor_seq[peer][epoch]`, silent discard (stop; no persist, no unread, not compromised).
8. **D013 ingest classifier** — normal · gap · soft compromised · hard reject; reorder buffer before gap declaration. Uses `ingest_policy` (D038).
9. **Persist** — append `ThreadMessage`, update watermarks, set `transport` from ingress path.

Validate `thread_id` and `message_id` as UUID before filesystem / DB use.

### Ingest classification (normal · gap · soft compromised · hard reject)

After steps 0–6 above (size OK; below-floor already discarded in step 7). Classifier runs in **`ingest_policy=strict`** unless user chose **continue anyway** (D038 → `relaxed`; see § Relaxed ingest).

**Normal (accept):**

1. **Benign duplicate** — same `(message_id, sender_seq, session_epoch)` → ignore.
2. **Epoch advance** — `session_epoch` increases → reset per-epoch watermarks; accept as fresh stream (see § Peer reset).
3. **Contiguous tail** — `sender_seq == contiguous_peer_seq + 1` and `sender_seq > history_floor_seq[peer][epoch]`.
4. **Tail bootstrap** — per-epoch transcript empty; ingest tail batch without requiring seq 1..N first (only seq **> floor** when floor is set).
5. **Authorized backfill** — `sender_seq` in `(history_floor_seq, loaded_min_seq)` only when user/system initiated history backfill for that range. After clear, this range is empty until new messages above floor exist locally.

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

**Hard reject (no continue-anyway — D038):**

| Condition | Why |
|-----------|-----|
| `session_epoch` **decreases** | Illegal rollback |
| Invalid signature / AEAD decrypt failure / wrong thread / envelope epoch mismatch | Wire or crypto invalid |

Reject message permanently (steps 2–4/6 already failed). Pause ingest/outbound if not already paused; show incident with **Pause only** — user must delete thread, rotate keys, or contact support. No relaxed ingest.

### Integrity recovery (D038)

**Detection ≠ policy.** Classifier stays strict by default; user picks response after disclosure.

**Default on soft compromised:** pause → choice sheet with:

1. **What we detected** (e.g. seq conflict at 42)
2. **Likely causes** (two devices, peer reset without epoch bump, relay oddity)
3. **Risk** (missing/reordered/duplicate messages; E2E: confidentiality/integrity may be broken)
4. **Recommended action** (primary button)
5. **Optional continue** (secondary/destructive styling)

**Channel-specific options:**

| Channel | Recommended | Optional |
|---------|-------------|----------|
| `e2e` | **Start new secure chat** — `user_resolution=rotate_psk`, `awaiting_new_psk`, manual PSK OOB, `session_epoch++`, optional `epoch_start` | **Continue with current keys** — `continue_anyway`, `ingest_policy=relaxed`, `trust_degraded=true` |
| `public_relay` | **Delete thread / start fresh** — `user_resolution=reset_thread` | **Continue anyway** — same relaxed flags |
| Both | **Pause only** — remain paused until another choice | — |

**E2E new secure chat flow** (`rotate_psk`):

Local state machine: `ok` → `gap` → `compromised` → `awaiting_new_psk` → `ok`.

```
Initiating side (recommended path)          Innocent peer
     |                                         |
     | 1. User confirms new secure chat        | 1. May see peer pause banner
     | 2. Exchange new PSK OOB                 | 2. Accept higher session_epoch
     | 3. session_epoch++ locally              |    on epoch_start or first seq msg
     | 4. Send epoch_start (seq=1) optional    | 3. Fresh watermarks for new epoch
     | 5. Resume at seq=2+                     |
```

- **Who bumps first:** initiating side after user confirms; innocent peer accepts **strictly higher** `session_epoch` (D014).
- **Old epoch keys:** retain for decrypting historical ciphertext only; do not ingest new traffic on old epoch after rotation.
- **Both sides compromised:** each user chooses independently; coordinated PSK exchange is manual (E001).

**Continue anyway** (`ingest_policy=relaxed`): see § Relaxed ingest. Show persistent **`trust_degraded`** badge until user rotates/resets or starts new epoch. Local override is not protocol agreement — peer may still be strict.

### Relaxed ingest (`ingest_policy=relaxed`, D038)

Active only when `user_resolution=continue_anyway` and `trust_degraded=true`.

| Situation | Rule |
|-----------|------|
| Seq conflict | Keep first-seen `(peer, epoch, sender_seq)`; discard conflicting inbound; no re-pause unless a third distinct `message_id` at same seq |
| Rewind / non-contiguous | Accept inbound; advance `contiguous_peer_seq` only on strict increase above current; gaps shown in UI, no auto-pause |
| Outbound | Re-enable sends on chat target |

User can return to strict policy only via **Start new secure chat**, **Delete thread**, or explicit reset — not silently.

### Clear history and seq (D037)

| Party | Behavior |
|-------|----------|
| **Sender** | `next_outgoing_seq` and `session_epoch` on chat target unchanged; next live send uses next seq as usual (e.g. 101) |
| **Receiver** | `DELETE FROM messages`; set `history_floor_seq[peer][epoch]` to max contiguous seq seen at clear time; reset `loaded_min`/`loaded_max`/`contiguous_peer_seq` watermarks for display (or derive from empty transcript) |
| **Below floor** | `sender_seq ≤ floor` in same epoch → **silent discard** on all paths (poll, direct, tail, gap, scroll). No persist, backfill, show, or unread bump. **No scroll resurrection** of cleared history. |
| **Above floor** | Normal D013 ingest — tail, gap repair, authorized history backfill in `(floor, loaded_min)` only |

The sender does not need a signal that the peer cleared locally; honest senders continue forward. Per-thread dedup (D034) is wiped with the transcript; seq floor — not a message-id registry — defines the sync boundary after clear. Full restart requires **epoch bump** (D014), not clear.

### Peer reset / new device (fresh stream)

When a peer wipes local state, installs on a new device without backup, or explicitly starts over:

1. **Bump `session_epoch`** on the chat target (mandatory — D014).
2. Reset `next_outgoing_seq = 1` for the new epoch only.
3. Optionally send `content_type=system`, `control_type=epoch_start` as the first relay-visible row (`sender_seq=1`); user content continues at seq 2+.
4. **Receiver** on unseen higher epoch: fresh per-epoch watermarks; `sender_seq=1` is normal bootstrap.

**Restored backup** (same identity + chat-target sidecar): not a reset — continue same epoch and seq.

Sending `sender_seq=1` without bumping epoch in an established epoch is **soft compromised** (D038 choice sheet; recommended path is epoch bump).

Benign duplicate delivery (same `message_id` + same `sender_seq`) is ignored via UUID dedup.

## Resource & trust bounds (D029–D033)

Canonical limits in [DECISIONS.md](DECISIONS.md) D029. Summary:

| Area | Policy |
|------|--------|
| Compose / send | Reject empty and > 64 KiB `text`; validate `ChatPayload` before send |
| Wire | Max 256 KiB envelope JSON; **no remote `content_rml`** (D030) |
| Storage | Size checks on insert; LRU of open `thread.db` (max 16) |
| UI | `GetMessagesPage` default 100 rows (D031) |
| Poll | Min **2 s** interval while foreground (D032); max 100 messages per batch |
| Outbox | Registry-backed; max 500 pending retry items |

**Local assistant `content_rml`** is trusted-local only (AI parser output), max 256 KiB on disk.

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
```

Send path: reject compose text and serialized payload over D029 limits before `AppendMessage`.

Keep `DeleteThread`, `AppendMessage`, `UpdateMessage`. Change `HasMessageId` to **`HasMessageId(thread_id, message_id)`** — per-thread `messages.id` lookup (D034); drop profile-global dedup from `JsonThreadStore` cutover.

## UI (target)

- **Sidebar groups (D023)** — collapsible sections: **AI**, **Public** (`public_relay` direct), **Private** (`e2e` direct). Same contact may appear in Public and Private.
- **E2E vs public shell** — `.chat-shell--e2e` / `.chat-shell--public` ([UI_DESIGN_SYSTEM.md](../../docs/UI_DESIGN_SYSTEM.md)).
- **Message row** — transport badge (E2E); delivery state; type-specific templates for `contact_card`, `crypto_tx`, `annotation`.
- **Windowed transcript (D031)** — render loaded page only; scroll-up fetches older messages; composer `maxlength` hint matching `kMaxComposeTextBytes`.
- **Sidebar list (D035)** — `ListThreads` returns catalog rows; verify/repair **visible rows only** when the sessions pane is shown or scrolled; pass viewport bounds from UI when virtualized.
- **Gap banner** — when `sync_state=gap`; tap to retry sync.
- **Integrity banner (D038)** — when `sync_state=compromised` or `trust_degraded`; choice sheet with disclosure + channel-specific options (recommended vs continue anyway).
- **Scroll hint** — when `loaded_min_seq > 1`.
- **`@ai` modes** — composer hints; confirm for `@ai+` / `@ai++`.
- **Clear history (D024)** — choice sheet: clear messages / clear messages & memory / delete conversation.
- **Forget AI memory** — separate action (`memory` table only).

## Non-goals (for now)

- Full cross-device history mirror (tail + scroll backfill only; see P2P sync above)
- **Multi-device concurrent send** on one identity (D015 — single active device v1)
- **Legacy on-disk migration** from pre-D028 JSON layouts (D016)
- Full-text search UI (defer; SQLite FTS across thread DBs later)
- Group E2E
- Retraction / “unsend” on relay (future protocol work)

## Success criteria

- [ ] All AI sidebar threads persist across restart via `IThreadStore` (no orphan `Conversation`-only path).
- [ ] User can clear history without deleting thread metadata.
- [ ] User can forget AI memory without losing visible transcript (and vice versa).
- [ ] Same contact can have separate public and E2E threads.
- [ ] Message IDs stable; relay dedup works; annotations reference targets by ID.
- [ ] E2E thread shows relay vs direct per message when transport is known.
- [ ] Direct threads assign `sender_seq` on send; receiver detects tail gaps and auto-repairs (direct + relay D027).
- [ ] Sidebar shows AI / Public / Private groups (D023).
- [ ] Clear history choice sheet implements D024 levels.
- [ ] `ChatPayload` types render: text, annotation, contact_card, crypto_tx (D026).
- [ ] Pending `relay_visible` messages survive restart and retry with same `(message_id, sender_seq)` (D017).
- [ ] Clear history preserves seq counters and epoch; `sender_seq ≤ history_floor` → silent discard, not compromised (D037).
- [ ] Peer reset bumps `session_epoch`; `sender_seq=1` on new epoch accepted; same-epoch rewind triggers integrity choice sheet (D038).
- [ ] Soft integrity failure pauses ingest/outbound; choice sheet offers recommended + continue anyway; hard wire failures have no override (D038).
- [ ] `continue_anyway` sets `ingest_policy=relaxed`, `trust_degraded`; relaxed rules per D038; incidents logged in `sync_state`.
- [ ] Duplicate `(sender, session_epoch, sender_seq)` with conflicting `message_id` triggers integrity UX with recommended rotation (`e2e`) or thread reset (`public_relay`).
- [ ] `@ai` local vs `@ai+` / `@ai++` shared modes behave per routing table; shared rows use trigger user’s sync seq.
- [ ] Transcript display order follows D019; reorder buffer (D020) prevents false gaps during benign reorder.
- [ ] D029 limits enforced on send/ingest; D030 no remote `content_rml`; D031 windowed UI; D032 poll backoff.
