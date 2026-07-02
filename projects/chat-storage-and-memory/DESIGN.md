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
| P2P sync (E2E) | tail + gap repair + **user sync** (D059) | scroll-driven history backfill |
| Integrity recovery | private: rotate PSK or pause | public/group: relaxed ingest default (D046) |
| Sidebar | flat list + channel badge | optional grouped sections |
| Transport | persist `transport` column | per-message badge UI |
| Ingest | Private strict D013 only (`e2e`) | `e2e_public` / group: relaxed default (D046) when those tiers ship |

## Implementer constraints

When building **`[v1]`** phases, satisfy these so **`[post-v1]`** features plug in without schema migration or hot-path refactors:

| Rule | Why |
|------|-----|
| **Branch on `content_type`**, default `text` | Rich types add templates, not a new storage model |
| **Branch on `thread.channel`** for seq/sync | Both direct tiers → seq + sync (D045); **`e2e`** → strict D013; **`e2e_public`** → relaxed D046 (D089) |
| **E2E body only on direct wire (D090)** | **`body.e2e.payload_b64` only** — reject `public_relay`, `body.content_b64` |
| **Reject unknown `content_type` on relay ingest only** | Wire validator; local rows may use future types in dev |
| **Keep all `messages` columns** at schema creation | `display_order`, `chat_actions`, `target_message_id`, `generation`, `transport`, seq fields — nullable until used |
| **`display_order` on every message** (D054) | Unified UI pagination + transcript sort; `sender_seq` is sync-only |
| **`ChatTargetKey` on wire; local `thread_id` only** (D056, D079) | Peers route by sender **communicating identity** + `route.channel`; never exchange local `thread_id` or local `Contact.id` |
| **Single wire/crypto shape** (D016) | No `thread_id` on envelope; no dual AAD versions — legacy JSON/relay layout wiped |
| **Populate full `sync_state` watermarks in v6** | `loaded_min_seq` / `loaded_max_seq` needed for `[post-v1]` history backfill |
| **Implement `GetMessagesBySeqRange` in v6** | Store query for tail/gap/responder serve (D060) |
| **Implement `FetchChatTargetMessages` in v6** (D058) | Feature-layer: tail, gap, manual sync, scroll backfill share one fetch + ingest path |
| **Peer-direct history protocol** (D060) | libp2p `/pp-browser/chat-history/1.0.0`; relay D027 fallback |
| **Authoritative empty gap close** (D061, D067) | Never-published seq after successful empty fetch — not compromised; guard when higher seq already held; late fill after tail close |
| **Inbound find-only (private); public auto-create (D080)** | **`e2e`:** outbound shell only; inbound without row → reject. **`e2e_public`:** auto-create on first decrypt |
| **Compromised thread freeze** (D068) | Outbox retry disabled; gap sync paused; epoch bump cancels old-epoch pending |
| **Wire cutover in v2a-p2p** (D063, D090) | Final E2E envelope shape; v4 validates — no second wire break |
| **C++ type gates** (D066) | `display_order` in v2a-core; `RelayEnvelope` without `thread_id` in v2a-p2p |
| **Gap repair UI defer** (D065) | Renumber inside loaded window → defer refresh until anchor reconciled |
| **`sync_state.state_json` extensible** | `[post-v1]` relaxed ingest adds keys without DB bump |
| **Set `transport` at send/receive** | `[post-v1]` badge UI reads column |
| **Participant check on all inbound direct** | D027 auth model |
| **Do not hardcode “AI never relays” in store layer** | Shared `@ai` sets `relay_visible=true` on specific rows only |
| **`chat_payload` BLOB is canonical body** (D069/D087) | Denormalized `content_type` / `payload` / `text` / `control_type` written only via `ChatPayloadCodec` |
| **`envelope_version` on every relay envelope** (D072) | Independent wire evolution from `ChatPayload.payload_version` and SQLite `user_version` |
| **Cap `empty_closed_seqs` / use ranges** (D071) | Bounded `sync_state.state_json`; coalesce before append |
| **Production disk: migrate, don’t wipe** (D069) | D016 wipe is dev/pre-users only; shippable layouts use incremental `user_version` migrations |
| **Shared history request/response types** (D072) | One struct for relay GET and libp2p D060 — see [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) |
| **Ignore unknown envelope keys** (D073) | Forward-compatible wire extensions without dual-parser |

## Principles

1. **One transcript model** — UI, disk, and LLM context derive from the same message store (`ThreadMessage` / future extensions), not parallel in-memory shapes.
2. **Local source of truth** — write locally before send; relay rejections do not erase history ([P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md)).
3. **Three storage layers** — distinguish what the user sees, what fits in the LLM window, and what the agent remembers long-term.
4. **Tier isolation (D089)** — **private direct** (`e2e`) and **public direct** (`e2e_public`) with the same **communicating identity** are different threads with different PSK, seq, ingest policy, and memory boundaries. **Group** (`kind=group`) is a third tier — E2E with pairwise sender-keys (E022), UX-first policy. The same local **Contact** may own multiple threads (different identities and/or channels/tiers).
5. **Stable IDs on the wire** — `message_id` (UUID) for dedup and sync; **`ChatTargetKey`** `(peer_identity_kind, peer_identity_value, channel)` for direct P2P routing (D056, D079). **`thread_id`** and **`Contact.id`** are local only — not sent to peers.
6. **Sender sequence for E2E direct completeness** — in **both direct tiers** (`e2e`, `e2e_public`), peer-visible messages carry a per-sender monotonic `sender_seq` (in addition to UUID) so receivers detect gaps in the live tail. UUID remains the only message identity everywhere.
7. **Ingest policy by tier (D089)** — **`e2e` (private direct)** uses strict D013: **normal**, **gap**, **soft compromised**, or **hard reject**. Soft failures **pause** ingest and outbound and show a **choice sheet** with recommended recovery only (D038) — no “continue anyway” in `[v1]`. **`e2e_public`** and **`group`** use the same seq machinery but **relaxed ingest by default** (D046). Hard crypto failures are non-overridable on all tiers.
8. **Durable outbox** — `relay_visible` rows with `delivery=pending` or `failed` survive app restart; retries reuse the same `(message_id, sender_seq)` on E2E direct tiers (D017). **Send failure keeps a local copy** — peer sync (D058/D059) resolves **receive-side** gaps, not unsent outbound; user **retries send** or clears (D024).
9. **Unified E2E backfill** — tail sync, gap repair, and user-initiated sync use **`FetchChatTargetMessages`** (D058): peer-direct first (D060), relay fallback (D027). Applies to both **`e2e`** and **`e2e_public`**.
10. **Storage abstraction** — `IThreadStore` stays the seam; **`SqliteThreadStore`** per-thread `thread.db` + `threads/profile.db` (`threads` catalog + `outbox` + `chat_targets`, D028, D035, D036, D047) from v2a. No `index.json` or other JSON thread files.

## Three chat tiers (D089)

Product P2P chat has **three tiers**. **All three** encrypt message bodies E2E on the wire (relay sees ciphertext). Tiers differ by **priority** and **policy defaults**, not by whether encryption is used.

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Tier            │ kind    │ Wire (direct)     │ UI      │ Priority         │
├─────────────────┼─────────┼───────────────────┼─────────┼──────────────────┤
│ Private direct  │ direct  │ channel=e2e       │ Private │ Security first   │
│ Public direct   │ direct  │ channel=e2e_public│ Public  │ UX first         │
│ Group           │ group   │ route.kind=group  │ Group   │ UX first         │
└──────────────────────────────────────────────────────────────────────────┘
```

**Engineering posture:** On UX-first tiers (public direct, group), accept security tradeoffs when they conflict with fluency — then spend maximum engineering effort to recover security within those constraints (auto key init, rotation, history recovery, multi-device). On the private tier, never compromise security for UX.

### Tier policy matrix

| Dimension | Private (`e2e`) | Public (`e2e_public`) | Group (`kind=group`) |
|-----------|-----------------|------------------------|----------------------|
| **Key establishment** | Manual OOB PSK + mandatory fingerprint (E011) | Auto init — directory / in-band / hybrid KEM (E013, O007) | Auto pairwise keys on join (E022, O008) |
| **Send gate** | Block until `psk_verified_at` | Send when auto key ready; fingerprint optional | Send when membership keys ready |
| **Key rotation** | User-driven; recommend on compromise | Automatic; prefer **epoch-only** bumps; less frequent `rotate_psk` to ease history recovery | Rotate affected pair keys on membership change |
| **Retired PSK ledger** | Cap 8 epochs (D086) | Higher cap / longer retention (product tuning) | Per-pair ledgers |
| **`sender_seq` + sync** | Full D013 + D058 backfill | Same sync path; **relaxed** on soft failures (D046) | Per-sender seq in `(group_id, session_epoch)` scope (D076) |
| **Ingest on seq conflict** | Pause + rotate or pause only (D038) | `continue_anyway` / LWW default (D046) | Same as public direct |
| **Multi-device** | Unsupported v1 → compromise (D015) | Target: supported (D074) | Target: supported |
| **Inbound without shell** | Hard reject (D080) | Auto-create after decrypt | Auto on invite/join |
| **Compromise UX** | Hard pause; outbox frozen (D068) | Auto-recover / non-blocking banner where possible | Remove member + re-wrap pair keys |

### Wire channel values (direct)

| Value | Tier | Body on relay |
|-------|------|---------------|
| **`e2e`** | Private direct | AEAD ciphertext — “Secure message” |
| **`e2e_public`** | Public direct | AEAD ciphertext — “Message” |

**No other direct channel values** — reject `public_relay` and unknown channels (D090). All direct envelopes use **`body.e2e.payload_b64`**; **`sender_seq`** and **`session_epoch`** required on wire (D045).

### Phasing

| Tier | Maturity | Notes |
|------|----------|-------|
| **Private direct** | `[v1]` | v6 plan — strict D013, manual PSK, integrity UX, full send/receive |
| **Public direct — data model** | `[v1]` (v2b) | `ChatTargetKey` + `channel=e2e_public`, sidebar badge, separate `thread.db` — **send/receive gated** until auto-key (c3+) |
| **Public direct — functional** | `[post-v1]` | Auto-key (E013/O007), encrypted bodies, relaxed ingest default (D046) — ships **with** public tier, not a separate optional phase |
| **Group** | `[post-v1]` | Membership + pairwise crypto (E022) before ingest ships |

**v2b rule:** **Message** may create an `e2e_public` shell for routing/UI, but **compose/send and inbound persist stay disabled** until e2e-message-crypto **c3** delivers auto-key for `e2e_public`. **Secure message** (`e2e`) is the only functional direct tier in `[v1]`.

## Assumptions (v1)

| Assumption | Implication |
|------------|-------------|
| **Single active sender per identity** (D015) | One client per profile may send on a **private direct** (`e2e`) chat target at a time. Two devices with the same identity and PSK without coordination will emit conflicting `sender_seq` → compromised (D011). Document in UX; multi-device seq coordination is **`[post-v1]`** for **`e2e_public`** / group (D089). |
| **No legacy migration** (D016) | Legacy flat `threads/{id}.json`, pre-D028 layouts, and **legacy relay envelopes with `thread_id`** are not upgraded — **dev/pre-user builds** may wipe `{data_dir}/profiles/{id}/threads/` on bump. **Shippable layouts** use incremental SQLite migration (D069), not wipe. |
| **No encryption at rest** (D048) | `thread.db` and `profile.db` are plaintext SQLite on disk. E2E body confidentiality is on the wire only; local disk is trusted. SQLCipher / OS keychain for transcript encryption is out of scope for v1. |
| **Timestamps are display-only for ingest** | `timestamp` is not authoritative for ordering or replay; direct tiers use `sender_seq`. No clock-skew rejection in v1. |
| **User-initiated direct shells (private)** (D062, D080) | **Private direct** `chat_targets` rows are created by outbound user actions (**Secure message**, first compose send). **Private** inbound without a row is rejected. **`e2e_public`** auto-creates on first inbound decrypt. |

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

## Contact and communicating identity (D079)

Address book and wire routing use **different id spaces**:

| Concept | Scope | Example | On wire? |
|---------|-------|---------|----------|
| **`Contact.id`** | Local address book | UUID in `contacts.json` | **No** |
| **`ContactId`** | Identity handle on a contact | `{ kind: relay_user, value: "relay:abc123" }` | **Value** only, when used for messaging |
| **`ChatTargetKey`** | Long-lived P2P conversation | `(relay_user, "relay:bob456", e2e)` | **Implied** by `sender_contact_id` + `route` |
| **`local:self`** | Local transcript sentinel | Outbound row UI | **No** |

A **Contact** represents a person or entity (human or non-human later). **`Contact.ids[]`** may include `relay_user`, `peer_id`, `blockchain`, and `custom` entries — only some are **messaging identities**. v1 relay path uses **`ContactIdKind::RelayUser`** values on the wire.

**Thread binding rules:**

- One direct thread = one **fixed** `(peer_identity_kind, peer_identity_value, channel)` — chosen at creation; **never** switch identity mid-thread.
- Same local Contact, two messaging identities → **two unrelated threads** (separate seq, PSK, sidebar rows). **`[post-v1]`** UI may group sidebar rows under one contact name.
- Identity rotation (new `relay_user` id) → **new thread**; old thread remains historical. Key rotation **within** the same identity → same thread, **`session_epoch++`** (D014).
- **`threads.participant_contact_ids`** = local **Contact.id** (UI). **`chat_targets`** PK = communicating identity + channel.

**Wire field note:** JSON key **`sender_contact_id`** is historical naming — it carries the sender's **communicating identity value** (e.g. `relay:abc123`), not local `Contact.id`. Format: [D082](DECISIONS.md#d082--relay-user-communicating-identity-string-format).

**Signing keys (E016, D081):** Ed25519 envelope verify uses **`PeerSigningKeyStore`** keyed by **`(peer_identity_kind, peer_identity_value)`** — not `Contact.id`, not on wire. Directory supplies **`signing_public_key_b64`** at add-contact; lazy relay lookup on cache miss (including first inbound on `e2e_public` auto-create). See [e2e DESIGN § Peer signing keys](../e2e-message-crypto/DESIGN.md#peer-signing-keys-e016).

## Data model (target)

### Thread

| Field | Type | Notes |
|-------|------|-------|
| `id` | UUID | **Local only** — `thread.db` directory name and catalog PK. **Not on wire.** AI: new UUID per conversation. Direct: current shell id from `chat_targets.local_thread_id` (D056). |
| `kind` | `ai` \| `direct` \| `group` | Unchanged |
| `channel` | `e2e` \| `e2e_public` | **Direct only.** Private vs public E2E tier (D089). Replaces overloading `encrypted` alone |
| `participant_contact_ids` | string[] | Local **Contact.id** for direct (UI grouping) |
| `peer_identity_kind` | string | **Direct only.** `relay_user`, `peer_id`, … (`ContactIdKind`) |
| `peer_identity_value` | string | **Direct only.** Wire routable id (= `ChatTargetKey` value, D079) |
| `title`, `preview`, `updated_at`, `unread_count` | — | Sidebar metadata; cached in `profile.db` `threads` (D035) |
| `encrypted` | bool | Derived from `channel` ∈ { `e2e`, `e2e_public` } (keep for UI binding) |
| `session_epoch` | uint32 | **E2E direct only** (`e2e`, `e2e_public`). Denormalized cache; authoritative in `chat_targets` (D047) |

**Direct logical identity:** **`ChatTargetKey`** `{ peer_identity_kind, peer_identity_value, channel }` — never one thread for both channels or two messaging identities. **`thread_id`** is a local shell pointer only (D056).

### ChatTargetKey (direct P2P — D056, D079)

Canonical name for **`(peer_identity_kind, peer_identity_value, channel)`** — the **communicating identity** plus channel. Used in C++ (`ChatTargetKey`), `chat_targets` PK (seq, epoch, **PSK** — D084), ingest routing, and relay backfill. HKDF `info` uses **`channel` + `epoch` only** (E015) — identity scopes the `master_psk`, not the KDF label.

| Field | Type | Notes |
|-------|------|-------|
| `peer_identity_kind` | string / enum | `relay_user`, `peer_id`, … — v1 relay uses `relay_user` |
| `peer_identity_value` | string | Routable id, e.g. `relay:abc123`, libp2p peer id (D082) |
| `channel` | `e2e` \| `e2e_public` | Direct tier with that identity (D089) |

**Log/test string key:** `identity:{kind}:{value}|channel:{channel}` — human-readable label; not a separate on-disk store.

**`[post-v1]` group:** use separate **`group_id`** on wire (`route.kind = "group"`); not a `ChatTargetKey`.

### Chat target (long-lived, direct P2P)

Seq counters and session epochs are keyed to **`ChatTargetKey`**, not `thread_id`. Delete conversation wipes the local shell; **`chat_targets` row persists** (seq/epoch). Reopen may allocate a **new** `local_thread_id` (D056).

| Field | Scope | Notes |
|-------|-------|-------|
| `local_thread_id` | chat target | Current on-disk shell UUID; **local only**, not on wire; may change on delete/recreate |
| `next_outgoing_seq` | chat target | Monotonic uint64 per epoch; assigned at first local persist before send |
| `session_epoch` | chat target | Increment on compromise recovery, full device reset, or explicit new secure chat (D014) |
| `master_psk_b64` | chat target | E2E direct tiers only (`e2e`, `e2e_public`); `NULL` until PSK installed (D084) |
| `psk_fingerprint` | chat target | E2E only; BLAKE2b display (E011) |
| `psk_verified_at` | chat target | E2E only; unix ms when user confirmed OOB fingerprint (E011); send gate |
| `retired_psks_json` | chat target | E2E only; retired `(epoch, master_psk)` entries after `rotate_psk` (E018) |

Persist in **`profile.db` → `chat_targets`** (D047), updated under the same writer mutex as `outbox`.

**`FindOrCreateDirectThread(ChatTargetKey, participant_contact_id)`:** lookup `chat_targets` by `(peer_identity_kind, peer_identity_value, channel)`; if missing, allocate `local_thread_id`, insert row + catalog + `{local_thread_id}/thread.db`. Store **`participant_contact_id`** (local Contact.id) on `chat_targets` and `threads.participant_contact_ids`. If row exists but shell missing (post-delete), allocate **new** `local_thread_id`, update row, recreate catalog + `thread.db` — seq/epoch unchanged.

### Per-peer sync state (per thread, scoped by `session_epoch`)

Sync watermarks are keyed by **`(peer_identity_kind, peer_identity_value, session_epoch)`**. A new epoch starts a fresh stream; old-epoch state is retained for history display but not used for gap logic on the new epoch.

| Field | Notes |
|-------|-------|
| `contiguous_peer_seq[peer][epoch]` | Highest seq received with no holes in the active tail for this epoch |
| `loaded_min_seq[peer][epoch]`, `loaded_max_seq[peer][epoch]` | Oldest/newest peer seq present in local transcript for this epoch |
| `history_floor_seq[peer][epoch]` | Set on **clear visible history** — max peer `sender_seq` that was in the transcript (`loaded_max_seq` before delete, D037); seq at or below floor in the same epoch is **excluded from sync**: silent discard, not compromised |
| `sync_state` | `ok` \| `gap` \| `compromised` — E2E only; `compromised` means paused pending user resolution (D038). Internal sub-state **`awaiting_new_psk`** during OOB key exchange after user picks rotate — not a separate `user_resolution` value. |
| `user_resolution` | **`[v1]`:** `null` \| `rotate_psk` \| `pause_only` (D038). **`[post-v1]`:** adds `continue_anyway` (D046). |
| `empty_closed_seqs[]` | uint64[] in `state_json` — seq values authoritatively empty-closed (D061/D067); removed on late-fill persist |
| `integrity_incidents[]` | `{ kind, detected_at, detail }` — ring buffer, max **`kMaxIntegrityIncidents`** (10, D049) per `(peer, epoch)` |

### ThreadMessage

C++ struct in `src/base/messaging/ThreadTypes.h` must stay aligned with store columns (D066).

| Field | Type | C++ / phase | Notes |
|-------|------|-------------|-------|
| `id` | UUID | v2a | Client-generated; dedup on ingest |
| `thread_id` | UUID | v2a | |
| `chat_payload` | BLOB | **v2a** | Canonical **binary `ChatPayload`** (D069/D087); wire-aligned |
| `content_type` | enum | **v4** wire+store | Denormalized from `chat_payload`; `text`, `system` **`[v1]`** |
| `payload` | JSON object | **v4** | Denormalized `ChatPayload.payload` |
| `text` | optional string | v2a | Display snippet / search / plain fallback; AI raw for `text` turns |
| `content_rml` | optional string | v2a | Rendered blocks (AI assistant) |
| `user_payload` | optional string | v2a | LLM-only structured JSON (AI turns) |
| `chat_actions` | array | v2a | Indexed chips |
| `target_message_id` | optional UUID | schema v2a | For `annotation` **`[post-v1]`** |
| `sender_contact_id` | string | v2a | **Local rows:** `local:self`, `ai:assistant`. **Wire / peer rows:** communicating identity **value** (D079) |
| `display_order` | int64 | **v2a-core** (D066) | Monotonic UI sort key; assigned at persist (D054). **Not** on wire. |
| `timestamp` | int64 | v2a | Metadata / display hint; **not** transcript sort key (D054) |
| `relay_visible` | bool | v2a | `true` when sent to peer; see `@ai` modes |
| `delivery` | enum | v2a-p2p | local, pending, relayed, failed |
| `transport` | enum | **v4** | local, relay, direct |
| `control_type` | optional string | v4 | For `content_type=system` |
| `sender_seq` | optional uint64 | **v6** | E2E + `relay_visible=true` only (D045) |
| `session_epoch` | optional uint32 | **v6** | Must match envelope |
| `generation` | optional enum | **`[post-v1]`** | `user` \| `ai_on_behalf` — shared `@ai` |
| `seq_owner_contact_id` | optional string | **`[post-v1]`** | Trigger user for `ai_on_behalf` |
| `ai_invoke_mode` | optional enum | v2a local | `local` **`[v1]`**; shared modes **`[post-v1]`** |

### RelayEnvelope (C++ — D066)

Target struct in `ThreadTypes.h`. **v2a-p2p** removes legacy fields.

| Field | Phase | Notes |
|-------|-------|-------|
| ~~`thread_id`~~ | **removed v2a-p2p** | Reject on ingest (D016) |
| `envelope_version` | **v2a-p2p** | **1** in v1; signed (D072) |
| `message_id` | v2a-p2p | |
| `sender_relay_id` | v2a-p2p | |
| `sender_contact_id` | **v2a-p2p** | Sender **communicating identity value** on wire (D079, D082) — e.g. `relay:abc123` |
| `route` | **v2a-p2p** | `{ kind, channel }` — `e2e` \| `e2e_public` (D090) |
| `body.e2e.payload_b64` | **v2a-p2p** | E2E ciphertext blob (D063/D090) |
| `sender_seq`, `session_epoch` | **v6** | Both direct tiers (D045) |
| `timestamp`, `signature` | v2a-p2p | |

**Legacy (baseline code today):** flat `RelayMessageBody { text, content_rml }` + `thread_id` — deleted at v2a-p2p cutover, not migrated (D016).

**Sync seq rule:** `sender_seq` assigned when `channel` ∈ { `e2e`, `e2e_public` } and `relay_visible=true`. Local-only rows never consume sync seq.

**Not sequenced:** local `@ai`, local-only system rows.

**LLM context:** `content_type=text` plus selected `system` rows only.

### ChatPayload (unified message body — D026)

One schema for disk and wire: **binary `ChatPayload`** (D087) inside AEAD plaintext for all direct tiers (E010/D090).

**Wire (D063/D090):** Direct envelopes use **`body.e2e.payload_b64`** only. **v4** adds full validator (`system`, unknown-type reject, D030, size caps) — **same wire shape**, no second parser.

**Binary layout (v1 — D087/D088):** see [WIRE_SCHEMAS.md § ChatPayload](WIRE_SCHEMAS.md#chatpayload-v1--binary-d087d088) and [§ Wire profile](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088).

**`[v1]` validator** accepts `text` and `system` on inbound relay; rejects unknown `content_type` enum values.

**Logical v1 `text` example** (canonical: plain default, no format tail):

| Field | Value |
|-------|-------|
| `content_type` | `text` (enum `0`) |
| `text` | `"optional human-readable snippet"` (LenUtf8) |

| `content_type` | Maturity | Type tail (inline) | UI |
|----------------|----------|-------------------|-----|
| `text` | **`[v1]`** | omit when plain default; else `sub_version` + `format` | Normal bubble; `text` required |
| `system` | **`[v1]`** | `sub_version` + `control_type` + `detail` (LenUtf8) | Centered system line |
| `annotation` | **`[post-v1]`** | `annotation_type`, `target_message_id`; optional `value` | Inline on target; badge |
| `contact_card` | **`[post-v1]`** | `contact_id`, `display_name`; optional `relay_user_id`, `avatar_url` | Contact card chrome |
| `crypto_tx` | **`[post-v1]`** | `chain_id`, `asset`, `amount`, `direction`; optional `tx_hash`, `status`, `to_address` | Transaction card |

**`[post-v1]` logical examples** (documentation JSON — on-wire binary uses `payload_version` per D087, not `schema_version`):

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
    "relay_user_id": "relay:abc123"
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
| `ConversationSummary` | `thread.db` → `memory` table | Key **`summary`**; JSON shape below (D070) |
| Future fact rows | Same table (kv) | Key prefix **`fact:`** — **`[future]`** |

**`memory` key namespace (D070):**

| Key pattern | Maturity | Value schema |
|-------------|----------|--------------|
| `summary` | **`[v1]`** (v3) | `ConversationSummary` JSON |
| `fact:{uuid}` | **`[future]`** | TBD — structured fact row |

**`ConversationSummary` value** (stored at key `summary`):

```json
{
  "schema_version": 1,
  "version": 3,
  "text": "User prefers dark mode. Discussed project timeline…",
  "compacted_through_display_order": 142,
  "updated_at": 1719662400
}
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `schema_version` | integer | yes | **1** — bump on breaking summary shape change |
| `version` | integer | yes | Monotonic per thread; increments on successful compaction (D040) |
| `text` | string | yes | Summary prose; max **`kMaxSummaryBytes`** (8 KiB, D040) |
| `compacted_through_display_order` | integer | no | Eligibility cursor — avoids full-thread scan (D040) |
| `updated_at` | integer (unix ms) | yes | Last successful write |

## On-disk layout (target — D025, D028, D035, D036)

Profile-scoped storage:

```
{data_dir}/profiles/{profile_id}/
  threads/
    profile.db                     # threads catalog + outbox + chat_targets (D017, D035, D047)
    {thread_id}/
      thread.db                     # messages, memory, sync_state — authoritative transcript
      blobs/                        # [future] content-addressed attachments (D075); empty in v1
```

No `sync/chat_targets.json` — chat-target counters live in `profile.db` (D047).

**`blobs/` (D075):** Reserved for **`[future]`** large/binary payloads referenced from `ChatPayload` (media, files). Content-addressed by hash under `{thread_id}/blobs/{hash}`; not used in v1. Create directory lazily on first blob write.

**Thread exists** iff `{thread_id}/thread.db` is present. `profile.db` `threads` row is a list cache; repaired lazily on sidebar list (D035).

**Delete conversation:**

| Kind | `chat_targets` | `local_thread_id` | On-disk |
|------|----------------|---------------------|---------|
| **Direct** | **Keep** (seq, epoch) | New UUID on shell recreate (D056) | `profile.db` txn: delete `threads` + `outbox`; remove `{local_thread_id}/` dir |
| **AI** | n/a | Gone — new UUID on next “new chat” | Same txn + dir remove |

**Clear messages** keeps catalog row, `thread.db`, `chat_targets`, and current `local_thread_id`; only transcript (+ optional memory) wiped.

### `thread.db` schema (v1)

```sql
CREATE TABLE messages (
  id TEXT PRIMARY KEY,
  display_order INTEGER NOT NULL,    -- UI transcript sort + pagination (D054)
  sender_contact_id TEXT NOT NULL,
  chat_payload BLOB NOT NULL,        -- canonical binary ChatPayload (D069/D087); wire-aligned
  content_type TEXT NOT NULL,        -- denormalized cache — write only via ChatPayloadCodec
  payload TEXT NOT NULL,             -- denormalized typed fields as JSON for queries
  text TEXT,                         -- denormalized ChatPayload.text
  content_rml TEXT,                  -- local-only assistant RML (never from wire, D030)
  user_payload TEXT,                 -- local-only LLM structured JSON (AI turns)
  chat_actions TEXT NOT NULL DEFAULT '[]',  -- JSON array (TranscriptChatAction)
  timestamp INTEGER NOT NULL,
  relay_visible INTEGER NOT NULL,
  delivery TEXT NOT NULL,
  transport TEXT,
  sender_seq INTEGER,
  session_epoch INTEGER,
  target_message_id TEXT,            -- denormalized from annotation payload when present
  generation TEXT,
  seq_owner_contact_id TEXT,
  ai_invoke_mode TEXT,
  control_type TEXT                  -- denormalized from system payload; optional
);
CREATE INDEX idx_messages_display ON messages(display_order DESC);
CREATE INDEX idx_messages_seq ON messages(session_epoch, sender_contact_id, sender_seq)
  WHERE relay_visible = 1;
CREATE INDEX idx_messages_delivery ON messages(delivery) WHERE relay_visible = 1;

CREATE TABLE memory (
  key TEXT PRIMARY KEY,              -- e.g. "summary"
  value TEXT NOT NULL                -- JSON
);

CREATE TABLE sync_state (
  peer_identity_kind TEXT NOT NULL,
  peer_identity_value TEXT NOT NULL,
  session_epoch INTEGER NOT NULL,
  state_json TEXT NOT NULL,          -- watermarks, sync_state, user_resolution,
                                     -- empty_closed_seqs[] / empty_closed_ranges[] (D067, D071),
                                     -- integrity_incidents[] (D038, D049)
  PRIMARY KEY (peer_identity_kind, peer_identity_value, session_epoch)
);
```

**`sync_state.state_json` — closed-seq tracking (D071):**

| Key | Type | Notes |
|-----|------|-------|
| `empty_closed_seqs` | uint64[] | Singleton closed seqs; max **`kMaxEmptyClosedSeqs`** (128) before coalesce |
| `empty_closed_ranges` | `{min,max}[]` | Inclusive ranges after coalescing adjacent closed seqs |
| *(other keys)* | — | Watermarks, `user_resolution`, `integrity_incidents[]`, etc. |

On append to `empty_closed_seqs`: **coalesce consecutive values into `empty_closed_ranges`** first. **Late fill** (D067) checks membership in either structure. **`[post-v1]` group:** 1:1 uses `(peer_identity_kind, peer_identity_value)` as scope; groups will generalize to `(scope_kind, scope_id, session_epoch)` (D076).

`PRAGMA user_version` on each `thread.db` and `profile.db` — see § Schema evolution (D069).

### `profile.db` schema (v1)

```sql
CREATE TABLE threads (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL,                -- ai | direct | group
  channel TEXT NOT NULL DEFAULT '',  -- e2e | e2e_public; empty for ai/group v1
  group_id TEXT,                     -- [post-v1] when kind=group; NULL in v1 (D076)
  peer_identity_kind TEXT,           -- direct only (D079)
  peer_identity_value TEXT,          -- direct only; wire routable id
  title TEXT NOT NULL,
  participant_contact_ids TEXT NOT NULL,  -- JSON array — local Contact.id(s)
  preview TEXT,
  updated_at INTEGER NOT NULL,
  unread_count INTEGER NOT NULL DEFAULT 0,
  session_epoch INTEGER              -- E2E direct denorm; authoritative in chat_targets
);
CREATE INDEX idx_threads_updated ON threads(updated_at DESC);
CREATE INDEX idx_threads_direct ON threads(kind, channel, peer_identity_kind, peer_identity_value);

CREATE TABLE outbox (
  message_id TEXT PRIMARY KEY,
  thread_id TEXT NOT NULL,
  delivery TEXT NOT NULL,            -- pending | failed
  updated_at INTEGER NOT NULL
);
CREATE INDEX idx_outbox_thread ON outbox(thread_id);
CREATE INDEX idx_outbox_updated ON outbox(updated_at ASC);

CREATE TABLE chat_targets (
  peer_identity_kind TEXT NOT NULL,
  peer_identity_value TEXT NOT NULL,
  channel TEXT NOT NULL,             -- e2e | e2e_public
  participant_contact_id TEXT,       -- local Contact.id; optional catalog link (D079)
  local_thread_id TEXT NOT NULL,     -- current on-disk shell; local only (D056)
  session_epoch INTEGER NOT NULL DEFAULT 1,
  next_outgoing_seq INTEGER NOT NULL DEFAULT 1,
  master_psk_b64 TEXT,               -- e2e + e2e_public; NULL until PSK installed (D084)
  psk_fingerprint TEXT,            -- e2e + e2e_public; BLAKE2b display (E011)
  psk_verified_at INTEGER,           -- e2e: send gate; e2e_public: optional (D089); unix ms
  retired_psks_json TEXT,            -- e2e + e2e_public; JSON [{epoch, master_psk_b64, retired_at}] (E018)
  PRIMARY KEY (peer_identity_kind, peer_identity_value, channel)
);
CREATE UNIQUE INDEX idx_chat_targets_local_thread ON chat_targets(local_thread_id);
```

**Scope:** `profile.db` holds the **sidebar list cache** (`threads`), **durable outbox index** (`outbox`), and **chat-target state** (`chat_targets` — seq, epoch, **PSK**, D047/D084). It does **not** store message-id dedup state (D034).

**Per-thread dedup:** After resolving inbound **`ChatTargetKey` → `local_thread_id`** (D056), `HasMessageId(local_thread_id, message_id)` → `SELECT 1 FROM messages WHERE id = ?`. Outbox retries use stored `local_thread_id`. **Clear history** wipes dedup surface with transcript rows.

Startup durable outbox scan → `SELECT * FROM outbox` (D017), then **reconcile** against `thread.db` (see § Startup reconciliation). Purge `outbox` rows on `DeleteThread`.

### Startup reconciliation (D047)

Run once per profile open after `ListPendingOutbox`:

| Check | Action |
|-------|--------|
| Outbox row, no message in `thread.db` | Delete orphan outbox row; log warning |
| Outbox row, message exists, `delivery=relayed` | Delete stale outbox row |
| Message `delivery=pending`/`failed`, no outbox row | Insert outbox row (repair from authoritative `thread.db`) |
| `chat_targets` row missing for known direct peer | Insert with new `local_thread_id` + defaults `(epoch=1, next_outgoing_seq=1)` or derive seq from max local outbound |
| `chat_targets` row present, catalog/`thread.db` missing | Allocate new `local_thread_id`, update row, recreate catalog + empty `thread.db` (D056) |

Optional dev-only: `PRAGMA integrity_check` on `profile.db` and open `thread.db` files; on failure offer delete-thread recovery.

### Epoch bump transaction (D014, D068, cross-project)

Single **feature-layer coordinator** flow when user starts new secure chat or peer reset requires **local-initiated** epoch bump. Hold **`profile.db` writer mutex** for all `chat_targets` updates (seq, epoch, PSK — D084) — no separate crypto sidecar.

**Passive adopt (D085):** when the **peer** bumps first, the innocent device performs the **same durable `chat_targets` / outbox / `sync_state` effects** on **first successful ingest** at `envelope.session_epoch > local` — inside step 12 persist, not via this coordinator. See § Passive epoch advance.

1. **Cancel old-epoch outbound (D068):** In `thread.db`, remove or mark cancelled all `relay_visible` rows with `delivery=pending|failed` for the **previous** `session_epoch` on this chat target; purge matching **`profile.db` `outbox`** rows in the same dual-DB recipe (D044). User re-composes in the new epoch — do not auto-resend stale envelopes.
2. **`profile.db` transaction** (same txn as step 1 catalog/outbox writes where applicable):
   - Increment `chat_targets.session_epoch`; reset `next_outgoing_seq = 1`.
   - **`rotate_psk`:** append `{ epoch: <old>, master_psk_b64: <previous>, retired_at }` to `retired_psks_json`, then set new `master_psk_b64` + `psk_fingerprint` (E018/D083/D084).
   - **Epoch-only (D014, same PSK):** increment `session_epoch` only — no `retired_psks_json` entry.
   - Update cached `threads.session_epoch` in `threads` row (E2E direct).
3. Reset per-peer `sync_state` watermarks for the new epoch in `thread.db`; clear `sync_state=compromised` / `user_resolution` when rotation completes.

No separate JSON sidecar for seq or PSK state — durable counters and keys in `profile.db` `chat_targets`.

### Catalog consistency (D035)

| Concern | Rule |
|---------|------|
| Authority | `thread.db` exists → thread is real; messages table is source for preview text and last activity time when verifying |
| List cache | `profile.db` `threads` — fast sort/filter; may be stale until verify |
| `ListThreads` | Read catalog; **verify visible slice only** (open `thread.db`, check existence, refresh preview/`updated_at` if needed) |
| Profile open | Once per profile: `readdir` — orphan `thread.db` without catalog row → insert stub `threads` row |
| Orphan catalog row | No `thread.db` on visible verify → delete `threads` + `outbox` rows |
| `AppendMessage` | `thread.db` txn first; then `UPDATE threads` (`updated_at`, `unread_count`); preview refresh deferred to verify (active thread may update eagerly) |
| `ClearMessages` | Compute `history_floor_seq` from **max peer `sender_seq` in transcript** (`loaded_max_seq`, D037) **before** `DELETE FROM messages`; purge **`profile.db` `outbox`** rows for pending/failed sends; `UPDATE threads` set `preview=''`, `unread_count=0`; wipe messages + reset display watermarks; keep `memory`/`sync_state` tables |
| `FindOrCreateDirectThread` | Lookup **`chat_targets`** by `ChatTargetKey`; catalog via `threads.peer_identity_*` + `channel` (D055, D079) |

### Schema evolution (D069)

Disk layout and wire format evolve on **separate version axes** — see [WIRE_SCHEMAS.md § Versioning matrix](WIRE_SCHEMAS.md#versioning-matrix).

| Axis | Mechanism | Production policy |
|------|-----------|-------------------|
| **`thread.db` / `profile.db`** | `PRAGMA user_version` | **Incremental SQL migrations** per version step (`1→2`, `2→3`, …) |
| **Legacy JSON / pre-v1 SQLite** | — | **No upgrade path** — one-time wipe when adopting v2a (D016) |
| **`RelayEnvelope`** | `envelope_version` | New version + signing spec; reject unknown versions on ingest |
| **`ChatPayload`** | `payload_version` | Parser branches; unknown `content_type` rejected on relay ingest |
| **E2E AAD** | `aad_version` | [e2e-message-crypto](../e2e-message-crypto/DESIGN.md) |

**Shippable `user_version=1`** is the first layout that **must** survive app upgrades without data loss. Implement **`SqliteThreadStore::Migrate(from, to)`** (or equivalent) before public release.

**Migration rules:**

1. Each bump runs a **single forward migration** inside a transaction; bump `user_version` only after success.
2. Migrations must be **idempotent-safe** where practical (e.g. `CREATE TABLE IF NOT EXISTS` for new artifacts).
3. **`profile.db` and `thread.db` versions are independent** — a profile open migrates `profile.db` once, then each opened `thread.db` lazily.
4. **Breaking wire cutover (D016)** ≠ breaking disk layout — do not wipe user transcripts on disk version bump.
5. **Dev builds** may offer “reset local data” on unsupported `user_version`; production shows blocking error + export hint.

**Canonical message body on disk (D069):** `messages.chat_payload` holds the full **binary `ChatPayload`** (same bytes as wire decode / E2E decrypt output). Columns `content_type`, `payload`, `text`, `control_type`, and `target_message_id` are **denormalized caches** populated **only** by `ChatPayloadCodec::EncodeToRow` on write — never updated independently. Local-only columns (`content_rml`, `user_payload`, `chat_actions`) sit outside `ChatPayload`.

## Clear / forget semantics (user-facing — D024)

**Clear history** opens a **choice sheet** with two actions (labels illustrative). Choosing **Clear messages** opens a **confirmation dialog** before any data is deleted (D057).

| Action | Transcript | LLM window | `memory` table | Thread shell | Outbox / pending sends | Sidebar | P2P `history_floor_seq` |
|--------|------------|------------|---------------|--------------|------------------------|---------|-------------------------|
| **Clear messages** | wipe | empty | keep by default; optional checkbox **Also forget what AI learned** wipes `memory` | keep | **cancelled** — delete `profile.db` `outbox` rows; pending/failed `relay_visible` rows removed with transcript (D017) | `preview=''`, `unread_count=0` | E2E: max peer `sender_seq` in deleted transcript per `(peer, epoch)` (D037) |
| **Delete conversation** | gone | gone | gone | remove catalog + dir; **direct:** keep `chat_targets` (seq/epoch) (D056) | all outbox rows for thread removed | row removed | n/a |

**Forget what AI learned** (separate menu item): transcript unchanged; wipe `memory` table only.

| Other action | Transcript | Memory | Notes |
|--------------|------------|--------|-------|
| **New chat** (AI) | new empty thread dir | empty | n/a |

P2P clear copy notes peer and relay may retain copies. Clear visible levels do **not** reset outgoing seq or `session_epoch` (D010).

### Clear messages — confirmation dialog (D057)

Shown **after** the user picks **Clear messages** on the choice sheet (and after any **Also forget what AI learned** checkbox state is set). **Confirm** runs `ClearMessages`; **Cancel** returns without changes.

Build the summary from a **pre-clear scan** of `thread.db` (and `profile.db` `outbox` for this `thread_id`) so counts are accurate.

**Title (illustrative):** `Clear message history?`

**Body sections** — include every section that applies; omit empty sections:

1. **Messages on this device**
   - Total message rows to delete (all senders: you, peer, AI assistant, system).
   - **E2E direct:** note that this includes messages **filled in by gap repair** (seq ranges that were not contiguous at receive time) — they are cleared like any other visible row.
   - **Local-only rows:** count of `@ai` assistant replies and other `relay_visible=false` rows (peer never saw these).

2. **Unsent and failed outbound** (if any `delivery=pending` or `delivery=failed` with `relay_visible=true`)
   - Count and short preview of each (truncated text).
   - **E2E:** state that assigned **`sender_seq` is not reused** — those sends are cancelled; your next successful send uses the next seq as usual (D010).
   - **Public direct (`e2e_public`):** state that cancelled sends will **not** be retried automatically; peer will not receive them.

3. **AI memory** — **one of:**
   - **When forget-AI checkbox checked:** state that the durable **conversation summary** in the `memory` table will be deleted; the AI will not retain compacted context from earlier turns. Transcript is already covered in §1.
   - **When forget-AI checkbox unchecked (D064):** **AI memory retained** — the visible transcript will be cleared but the **conversation summary** in the `memory` table **remains**; the AI may still use compacted context from earlier turns in future replies. To wipe memory too, check **Also forget what AI learned** or use **Forget what AI learned** separately.

4. **What stays**
   - Thread remains in the sidebar (title unchanged).
   - **Direct:** `chat_targets` seq/epoch unchanged; new messages still work.
   - **E2E:** `history_floor_seq` updated so **tail sync, gap repair, poll, and user sync** will not bring back cleared seq (including repaired gaps) in this epoch (D037).
   - **AI (when forget unchecked):** `memory` table unchanged (see §3).

5. **What this does not do**
   - Does **not** delete messages on the peer's device or on the relay.
   - Does **not** reset `session_epoch` or outgoing seq counters — for a full cryptographic restart, use **Start new secure chat** (E2E, v6).
   - Does **not** remove the thread — use **Delete conversation** for that.

**Footer actions:** `Cancel` · `Clear messages` (destructive emphasis).

**Delete conversation** may use a shorter confirmation (whole thread + memory removed); no need to repeat the full inventory unless product prefers parity.

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

- Trigger user owns **`seq_owner_contact_id`** and **`sender_seq`** on the wire; envelope **`sender_contact_id`** = trigger user's **communicating identity value** (e.g. `relay:…`, D079).
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

## Relay / direct envelope (D056, D063)

**No `thread_id` on the wire** — each peer keeps a local `thread_id` / `local_thread_id` only. Direct P2P routing uses **`sender_contact_id`** (communicating identity **value**, D079) + **`route`** (D056). Reject legacy envelopes with `thread_id`, `public_relay`, `body.content_b64`, or flat `body.text` (D016/D090).

### Wire cutover phasing (D063)

| Phase | Envelope | Body | Notes |
|-------|----------|------|-------|
| **Baseline (today)** | includes `thread_id` | flat `text` / `content_rml` | Removed at v2a-p2p — wipe data (D016) |
| **v2a-p2p** | **`envelope_version`**, `sender_contact_id`, `route`, no `thread_id` | **`body.e2e.payload_b64`** (E2E blob; decrypt in c2) | D063/D090 — reject `public_relay`, `content_b64` |
| **v4** | unchanged | same shape | Add validator: `system`, reject unknown types, D030, D029 limits |
| **v6** | + `sender_seq`, `session_epoch` | unchanged | AAD + signature bind seq fields |

**Private direct (`e2e`)** — `route.channel` = `e2e`; `body.e2e.payload_b64`; `sender_seq`, `session_epoch` on wire (v6+):

```json
{
  "envelope_version": 1,
  "message_id": "uuid",
  "sender_relay_id": "relay:alice123",
  "sender_contact_id": "relay:alice123",
  "route": { "kind": "direct", "channel": "e2e" },
  "sender_seq": 42,
  "session_epoch": 1,
  "body": { "e2e": { "payload_b64": "…" } },
  "timestamp": 1719662400123,
  "signature": "…"
}
```

**Public direct (`e2e_public`)** — same shape; `route.channel` = `e2e_public`.

**Unknown-field policy (D073):** ingest **ignores** unknown top-level envelope keys (after required fields parse). **`ChatPayload` (binary):** reject unknown `content_type` on relay ingest; reject unknown typed sub-record fields in v1. Full rules: [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md).

**Normative wire shapes:** [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) — `RelayEnvelope` (JSON), binary `ChatPayload` (D087), `ChatHistoryRequest`, `ChatHistoryResponse`. C++ codecs and relay/libp2p glue share one struct pair for history fetch (D072).

**`[post-v1]` group** — `route`: `{ "kind": "group", "group_id": "group:…" }` (no `ChatTargetKey`).

| Field | Required | Notes |
|-------|----------|-------|
| `envelope_version` | yes | **1** in v1; included in signature canonical bytes (D072) |
| `message_id` | yes | UUID dedup (D034) |
| `sender_contact_id` | yes | Sender **communicating identity value** (D079, D082) — e.g. `relay:abc123` |
| `route.kind` | yes | `direct` **`[v1]`**; `group` **`[post-v1]`** |
| `route.channel` | yes when `kind=direct` | `e2e` \| `e2e_public` (D090) |
| `sender_seq`, `session_epoch` | yes on direct | Required for both tiers (D045) |
| `sender_instance_id` | **`[future]`** | Multi-device extension (D074); omit in v1 |

`payload_b64` decodes to `[payload_version:1][nonce:24][ciphertext+tag]`; AEAD plaintext is binary `ChatPayload` v1 (E010/D087).

**Inbound routing (direct):**

```
ChatTargetKey key = {
  peer_identity_kind:   relay_user,   // v1 relay path; infer from transport
  peer_identity_value:  envelope.sender_contact_id,
  channel:              envelope.route.channel   // e2e | e2e_public
};
// D080: no row — private reject; e2e_public auto-create
local_thread_id = chat_targets[key].local_thread_id;
```

- **`sender_contact_id`** required on the wire (D021). Value is the sender's **communicating identity** (D079) — not local `Contact.id`. Do not infer sender from local thread metadata alone.
- **Signature** covers **canonical binary sign bytes** (E014): `envelope_version`, `message_id`, `sender_contact_id`, `route`, `timestamp`, `body_hash` (BLAKE2b-256), `sender_seq`, `session_epoch` — **not** `thread_id` or `sender_relay_id`. Full layout: [e2e-message-crypto DESIGN § Ed25519 signing](../e2e-message-crypto/DESIGN.md#ed25519-canonical-signing-bytes). Reject `public_relay` (D090).

### Send pipeline

1. Resolve **`ChatTargetKey`** from local direct thread (peer + `channel`).
2. Serialize `next_outgoing_seq` assignment per key in **`profile.db`** (mutex, D047).
3. **Direct tiers:** assign `(message_id, sender_seq)` at first local persist when `relay_visible=true`.
4. Persist to **`local_thread_id`** with `delivery=pending` **before** network I/O.
5. Build envelope **without `thread_id`**; sign and send; on failure set `delivery=failed` and enqueue durable retry (D017).
6. Retries reuse the **same** ids — E2E: `(message_id, sender_seq)`; public: `message_id` only.

### Durable outbox (D017)

| Source | Behavior |
|--------|----------|
| On startup | `profile.db` `outbox` table + optional per-thread verify (D028) |
| In-memory queue | May batch IO; must not be the sole copy of pending state |
| Registry | On append `relay_visible` pending: insert `outbox`; on relayed: delete row |
| Retry policy | Exponential backoff; max **`kMaxOutboxRetryAttempts`** (D041) per message; user-visible notice on persistent failure; manual retry resets counter |
| **While compromised** (D068) | **No** auto-retry and **no** manual **Retry send** — outbox frozen until user rotates keys or deletes thread |
| Relay dedup | Server must accept duplicate `message_id` on retry (idempotent ingest) |

## P2P sync (E2E direct tiers — D045)

Both **`e2e`** and **`e2e_public`** use seq-scoped sync via **`FetchChatTargetMessages`** (D058).

### Sync modes

| Mode | Maturity | Trigger | Behavior |
|------|----------|---------|----------|
| **Tail sync** | **`[v1]`** | Open E2E thread, reconnect, new device | Fetch latest **N** peer-visible messages per sender (default **50**) |
| **Gap repair** | **`[v1]`** | Hole in contiguous tail (`seq N` + `seq N+2+`) | Auto backfill via D058; not scroll-gated |
| **User-initiated sync** | **`[v1]`** | Thread menu **Sync with peer**; gap banner **Retry sync** (D059) | Tail refresh + repair known gaps + older range when `loaded_min_seq > floor + 1` |
| **History backfill (scroll)** | **`[post-v1]`** | User scrolls to top of loaded transcript (D052) | Page `(history_floor_seq, loaded_min_seq)` via D058; **25** per page |

**`[v1]` scroll-up:** `GetMessagesPage` on **local** transcript only — no automatic fetch on scroll (D052). User may use **Sync with peer** for older history.

**Outbound vs inbound:** Rows with `delivery=pending`/`failed` are **local unsent** — fix with **retry send** or clear (D024), not peer sync. Peer sync fetches messages **the other party published**.

### Unified backfill — `FetchChatTargetMessages` (D058)

Feature-layer API (name illustrative). All E2E sync modes call this; do not duplicate relay/direct HTTP in each trigger.

**Input:**

| Field | Notes |
|-------|-------|
| `ChatTargetKey` | `(peer_identity_kind, peer_identity_value, channel)` |
| `session_epoch` | Required for E2E |
| `min_sender_seq` | Inclusive; use `history_floor_seq + 1` when floor set (D037) |
| `max_sender_seq` | Optional inclusive upper bound |
| `limit` | Default **50**, max **100** (D029) |
| `order` | `asc` or `desc` |

**Transport order:**

1. **libp2p peer-direct** — protocol D060 when session connected to peer.
2. **Relay** — `GET /v1/chat-targets/messages` (D027) on direct failure, timeout, or peer offline.

**Output:** Ingest each returned `RelayEnvelope` through the receive pipeline (D013, D037). Update `loaded_min_seq` / `loaded_max_seq` / `contiguous_peer_seq` on success.

**Mode-specific ranges** (when `history_floor_seq` is set, `min_sender_seq = floor + 1`):

| Mode | Typical request |
|------|-----------------|
| Tail sync | `min_sender_seq=floor+1`, `order=desc`, `limit=50` |
| Gap repair | `min_sender_seq=max(N+1, floor+1)`, `max_sender_seq=M`, `order=asc` |
| User-initiated sync | Tail + any known gap ranges; if `loaded_min_seq > floor+1`, also `max_sender_seq=loaded_min-1`, `limit=25`, `order=desc` |
| Scroll backfill **`[post-v1]`** | Same as user-initiated older range; scroll trigger only |

Discard below-floor rows without compromising (D037).

**`[post-v1]` scroll request** (when floor set):

```
min_sender_seq=floor+1, max_sender_seq=loaded_min-1, limit=25, order=desc
```

Ingest: authorized backfill only — seq in `(history_floor_seq, loaded_min_seq)` when user initiated fetch. UX: scroll hint when `loaded_min_seq > 1` and direct/relay may have older rows.

### User-initiated sync UX (D059)

**Thread menu (E2E direct):** **Sync with peer** — runs tail + gap repair + one older-history page if applicable. Show progress; on success refresh `GetMessagesPage`.

**Gap banner:** **Retry sync** invokes gap repair subset of D058 before escalating to compromised.

**Copy:**

- Sync fixes **missing messages from your peer** (receive-side / older history).
- **Unsent / failed** messages on this device need **Retry send** (or clear) — peer sync does not upload your pending outbox. **Exception:** while `sync_state=compromised`, outbox retry is disabled (D068) — use **Start new secure chat** or **Delete conversation**.

**Clear history (D037):** User sync must **not** resurrect seq ≤ `history_floor_seq` in the same epoch.

### Peer-direct history fetch (D060)

libp2p stream protocol **`/pp-browser/chat-history/1.0.0`**. Semantics mirror D027; only transport differs.

**Request** (UTF-8 JSON, requester signs canonical bytes or uses established libp2p identity binding):

```json
{
  "requester_identity_kind": "relay_user",
  "requester_identity_value": "relay:…",
  "peer_identity_kind": "relay_user",
  "peer_identity_value": "relay:…",
  "channel": "e2e",
  "session_epoch": 1,
  "min_sender_seq": 10,
  "max_sender_seq": 42,
  "limit": 50,
  "order": "asc"
}
```

**Response:**

```json
{
  "peer_identity_kind": "relay_user",
  "peer_identity_value": "relay:…",
  "channel": "e2e",
  "session_epoch": 1,
  "messages": [ /* RelayEnvelope[] — no thread_id */ ],
  "has_more": false,
  "cursor": { "next_min_sender_seq": null, "next_max_sender_seq": null }
}
```

**Responder rules:**

- Verify requester is the other party to `ChatTargetKey` (1:1 participant).
- Read from local `GetMessagesBySeqRange` on **`chat_targets.local_thread_id`** for the requested sender stream(s).
- Cap `limit` at **`kMaxPollBatchMessages`** (D029).
- Return full signed envelopes (same as stored / relay would return).

**Requester rules:** Verify envelope signatures; ingest via receive pipeline; set `transport=direct` on persisted rows.

Implementation lives in `src/libp2p/integration/host/` + `P2pMessagingService` — not in `IThreadStore`.

### Within-epoch sender contract (E2E only)

For a fixed chat target `(peer_identity_kind, peer_identity_value, channel, session_epoch)`, the **sender** must obey:

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
- **Contiguous gap:** local state for this epoch already has seq **N** and receives **N+2+** above `history_floor_seq` → `sync_state=gap`, attempt repair via D058 (not yet compromised). Repair requests clamp to **`kMaxGapRepairSeqSpan`** (D041). **Authoritative empty success** for the gap range → close hole per D061 **only when D067 guard passes** (not a failed round). **Transport failures** increment toward **`kMaxGapRepairRounds`** → soft compromised (D038).
- **While `sync_state=compromised` (D068):** do **not** run auto gap repair, tail sync, or **Sync with peer** — integrity choice sheet only.

E2E backfill is **peer-first** (D060); **relay** (D027) when peer offline or direct unavailable.

### Relay API — chat-target message fetch (D027, D056)

**Relay fallback** for **`FetchChatTargetMessages`** (D058) when peer-direct (D060) is unavailable. Authenticated as relay user.

**Authorization (required):** Relay MUST verify the authenticated caller is a **party to the requested `ChatTargetKey`** (1:1: `peer_identity_value` is an identity they may message; **`[post-v1]`** group: member of `group_id`). Non-participants receive **403**. Client ingest MUST reject when `sender_contact_id` does not match the thread's bound **`peer_identity_value`** (and kind).

**`GET /v1/chat-targets/messages`**

| Query param | Required | Description |
|-------------|----------|-------------|
| `peer_identity_kind` | yes | Other party's identity kind (v1: `relay_user`) |
| `peer_identity_value` | yes | Other party's communicating identity (stream owner for fetch) |
| `channel` | yes | `e2e` \| `e2e_public` |
| `session_epoch` | yes (E2E) | Epoch scope |
| `min_sender_seq` | no | Inclusive lower bound (gap repair, E2E) |
| `max_sender_seq` | no | Inclusive upper bound (gap repair, E2E) |
| `limit` | no | Default **50**, max **100** |
| `order` | no | `asc` (default) or `desc` |

Relay stores messages by **(recipient inbox, sender_contact_id, channel)** — not client `thread_id`.

**Sync mode usage** (when `history_floor_seq` is set, use `min_sender_seq = floor + 1` on all modes):

| Mode | Typical request |
|------|-----------------|
| Tail sync | `min_sender_seq=floor+1`, `order=desc`, `limit=50` |
| Gap repair | `min_sender_seq=max(N+1, floor+1)`, `max_sender_seq=M`, `order=asc` |
| User-initiated sync | Tail + gap ranges; optional `max_sender_seq=loaded_min-1`, `limit=25`, `order=desc` |
| Scroll backfill **`[post-v1]`** | Same older-range params as user sync; scroll trigger only |

Discard any below-floor rows in relay responses without compromising (D037).

**Response 200:**

```json
{
  "peer_identity_kind": "relay_user",
  "peer_identity_value": "relay:…",
  "channel": "e2e",
  "session_epoch": 1,
  "messages": [ /* RelayEnvelope[] — no thread_id */ ],
  "has_more": true,
  "cursor": {
    "next_min_sender_seq": 10,
    "next_max_sender_seq": null
  }
}
```

- Each element is a full signed `RelayEnvelope` (client verifies signature; resolves `ChatTargetKey`; **E2E** runs D013 ingest).
- **`POST /v1/messages`** (or existing send): idempotent on `message_id` — duplicate POST returns 200 with same id (D017). Reject body > `kMaxRelayEnvelopeJsonBytes` (D029). **Reject** bodies containing `thread_id`.
- Inbox **poll** may remain for notifications; clients must not rely on poll alone for seq-complete history. Max **100** messages per poll response (D029/D032).

MCP bridge: expose equivalent `relay_fetch_chat_target_messages` tool with same parameters.

### Reorder buffer (D020) — E2E only

Implemented via **`ReplayWindow`** helper in `base/crypto` ([e2e-message-crypto](../e2e-message-crypto/DESIGN.md)); **D013 classifier in the feature layer is authoritative** — `ReplayWindow` holds out-of-order slots only; it does not persist or override compromise policy.

During gap repair or multi-path delivery (direct + relay), E2E messages may arrive out of order:

- **`kReorderWindow = 32`** — hold inbound messages with `sender_seq` in `(contiguous_peer_seq, contiguous_peer_seq + kReorderWindow]` before declaring `gap`.
- Messages above the window without filling the hole → `sync_state=gap`, trigger repair.
- After repair, flush buffer in seq order before updating `contiguous_peer_seq`.

### Display ordering (D019, D054)

**UI transcript sort** (all channels): `display_order ASC`. `BuildDisplayRows` and `GetMessagesPage` use this column only — no runtime merge pass.

**Sync / ingest** (separate from UI):

| Channel | Ordering authority |
|---------|-------------------|
| **E2E direct** (`e2e`, `e2e_public`) `relay_visible` | `(session_epoch, sender_contact_id, sender_seq)` — gap detection, `GetMessagesBySeqRange`; UI still sorts by `display_order` (D054) |
| **Local-only** (`relay_visible=false`) | `display_order` at persist — interleaves with relay rows in UI |
| **AI threads** | `display_order` at persist only |

`timestamp` is metadata only — not used for transcript pagination or sort (D054).

### Scroll stability and gap-repair refresh (D065)

UI scroll anchor is always **`message_id`**, never array index or stale `display_order` after renumber.

**Before committing** a gap-repair batch (D054 Rule 2), compute the set of **`message_id`** values whose `display_order` will change and compare to the loaded **`GetMessagesPage`** window:

| Situation | UI behavior |
|-----------|-------------|
| All repaired/renumbered rows are **above** `min(loaded display_order)` | Persist only; **skip** display refresh — user sees no jump. |
| Any affected row is **inside** the loaded window | Set **`defer_display_refresh`**; after txn commit, re-resolve anchor `message_id`, call `GetMessagesPage`, **`BuildDisplayRows`**, restore scroll to anchor. |
| Live append while user is scrolled up | Default Rule 1 append at tail — no refresh required unless active window includes tail. |

`ChatController` owns anchor state. Do not re-sort the in-memory row vector by array index after repair — always re-fetch page by store cursor.

### Display order assignment (D054)

Assigned inside **`AppendMessage`** (send, receive, gap repair, local `@ai`):

**Rule 1 — Default append** (in-order send, poll, tail ingest, local `@ai`, AI/public threads):

```
display_order = max(display_order in thread) + 1
```

**Rule 2 — E2E gap repair** (relay-visible row with `(session_epoch, sender_contact_id, sender_seq)` between existing seq neighbors):

1. Find prev/next neighbor on that sender’s stream (same epoch) via `GetMessagesBySeqRange`.
2. Assign `display_order` values strictly between `prev.display_order` and `next.display_order`.
3. If integer gap is too small for the batch, **renumber** tail rows’ `display_order` in the same `thread.db` txn — prefer one contiguous renumber pass per repair batch (not per row) to limit scroll churn (see § Scroll stability and gap-repair refresh, D065).

Local-only rows always use Rule 1.

**Rule 3 — Clear messages:** `display_order` resets with `DELETE FROM messages` (empty transcript).

Per-thread **`max_display_order`** may be cached in `thread.db` metadata or derived from `MAX(display_order)` on append.

**Complexity budget (D077):** Rule 2 integer insert/renumber is the **v1 display-order model** — expect ongoing edge-case maintenance (D065). **`[future]` escape hatches** (not scheduled): string **lexicographic order keys** (fractional indexing) or a separate **`ui_order`** graph keyed by `message_id` to decouple gap-repair ingest from mass renumber.

### Receive pipeline

Ordered steps — **single linear sequence**; do not reorder in implementation (D022, D033, D056). **All direct tiers** run the full pipeline including decrypt (D090).

0. **Envelope size** — reject if serialized JSON > `kMaxRelayEnvelopeJsonBytes` (D029).
1. **Reject legacy shape** — if `thread_id` present → hard reject (D016). Require **`envelope_version=1`**; reject unknown envelope versions (D072).
2. **Verify Ed25519 signature** on outer envelope (classical; E014). Resolve **`signing_public_key_b64`** from **`PeerSigningKeyStore`** by `(peer_identity_kind, envelope.sender_contact_id)`; on miss, lazy **`GET /v1/users/{relay_user_id}`** (E016, D081). **Fail closed** if key missing or verify fails.
3. **Parse `route`** — `kind=direct` requires `channel`; unknown `kind` → reject.
4. **Resolve local thread (direct)** — build **`ChatTargetKey`** from `envelope.sender_contact_id` (value) + inferred **`peer_identity_kind`** (v1: `relay_user`) + `envelope.route.channel` → lookup **`chat_targets`**. **Inbound (D062, D080):**
   - **`e2e` (private):** no row → **hard reject** (stop). Row exists but shell missing → hard reject.
   - **`e2e_public` (public):** no row → set **`pending_auto_create=true`**, continue (decrypt + auto-create at step 12 — § Inbound auto-create). Row exists but shell missing → hard reject.
   - **Outbound** user send uses **`FindOrCreateDirectThread`** (never on private inbound).
   - **`[post-v1]` group:** `route.group_id` → group thread lookup.
5. **Per-thread UUID dedup** — when `local_thread_id` is known: `HasMessageId(local_thread_id, envelope.message_id)`; benign duplicate → stop (D034). Skip when `pending_auto_create` (no shell yet).
6. **Participant check** — when a **`chat_targets`** row exists: `envelope.sender_contact_id` must equal the row's **`peer_identity_value`** (and kind matches). When **`pending_auto_create`** (`e2e_public` only): require valid Ed25519 verify (step 2) and **`PeerSigningKeyStore`** entry for sender (directory or prior add-contact); reject unknown senders — see § Inbound auto-create. Outbound reflected echo may use `local:self` in local rows only — not on wire.
7. **Decrypt** — resolve `master_psk` via **`IPskSessionStore::ResolveMasterPskForEpoch(envelope.session_epoch)`** (E018/D085 — **never** `chat_targets.session_epoch`, which may lag on passive adopt); for **`pending_auto_create`**, run auto-key init (E013/O007) to obtain or derive PSK before decrypt. Derive session key with HKDF `info` using **`envelope.session_epoch`** (E015); AEAD decrypt + verify AAD binds the same epoch. Failure → hard reject.
8. **Plaintext size** — decrypted binary ≤ `kMaxE2ePlaintextBytes`. Reject before `ChatPayloadCodec::Decode` (D033).
9. **Parse & validate `ChatPayload`** — **`[v1]`** types `text`, `system`; strip wire `content_rml` (D030).
10. **History floor (E2E only, D037)** — when `local_thread_id` exists: if `sender_seq ≤ history_floor_seq[peer][epoch]`, silent discard, stop. Skip when `pending_auto_create` (empty transcript).
11. **D013 ingest classifier (E2E direct tiers)** — normal · gap · soft compromised · hard reject; `ReplayWindow` before gap declaration (D020). **`e2e`:** strict only. **`e2e_public`:** relaxed default when tier is active (D046). When `pending_auto_create`, use **bootstrap / tail** rules only (empty per-epoch transcript). Skip when `sync_state=compromised` except to record incidents — do not persist new rows until resolved (D068).
12. **Persist** — when **`pending_auto_create`**: **`FindOrCreateDirectThread(ChatTargetKey)`** in the same dual-DB txn as first message row; init `sync_state` watermarks for the envelope epoch. Assign `display_order` (D054); append to **`local_thread_id`**; update watermarks; set `transport`. **Passive epoch adopt (D085):** when `envelope.session_epoch > chat_targets.session_epoch` and steps 7–11 succeeded, the same dual-DB transaction MUST also update `chat_targets.session_epoch`, reset `next_outgoing_seq = 1`, cancel old-epoch pending/outbox (D068), refresh `threads.session_epoch` denorm, and init `sync_state` for the envelope epoch — see § Passive epoch advance.

Validate `message_id` as UUID before DB use. Validate `local_thread_id` as UUID before filesystem use.

### Inbound auto-create (`e2e_public` only — D080)

First inbound on **`channel=e2e_public`** when no **`chat_targets`** row exists. **Do not** stop at step 4 — the full decrypt path runs first; shell allocation happens at step 12.

**Participant gate (before persist):**

| Check | Rule |
|-------|------|
| Envelope signature | Must pass step 2 (Ed25519) |
| Signing key | **`PeerSigningKeyStore`** must resolve key for `(peer_identity_kind, envelope.sender_contact_id)` — from add-contact directory hit or lazy relay fetch (D081) |
| Blocklist | **`[future]`** — reject if sender is blocked; v1: accept any verified sender (product may tighten via O007) |
| Spam / unknown sender | Show first-message UX (preview optional); user may delete thread after auto-create |

**Ordering (single txn at step 12):**

1. **`FindOrCreateDirectThread(ChatTargetKey, participant_contact_id?)`** — `participant_contact_id` linked when sender matches a local **Contact.ids[]** entry; else NULL catalog link until user adds contact.
2. Insert **`chat_targets`** with auto-key PSK columns (E013/D084), `session_epoch` from envelope, `next_outgoing_seq = 1`.
3. Init **`sync_state`** for `(peer, envelope.session_epoch)` with bootstrap watermarks.
4. Append message row; apply D013 classification used in step 11 (bootstrap/tail normal).

**Not allowed on auto-create path:** ephemeral-only preview without persist (supersedes pre-D090 behavior). **`e2e` (private)** never auto-creates — hard reject at step 4 when no row.

### Ingest classification (E2E direct tiers — normal · gap · soft compromised · hard reject)

After crypto/size checks; below-floor already discarded in step 10. **`e2e` (private) `[v1]`:** always strict — no relaxed override (D046). **`e2e_public`** and **`group`:** relaxed ingest default (D046). See § Integrity recovery.

**Normal (accept):**

1. **Benign duplicate** — same `(message_id, sender_seq, session_epoch)` → ignore.
2. **Epoch advance** — `envelope.session_epoch > chat_targets.session_epoch` → reset per-epoch watermarks for the **envelope epoch**; accept as fresh stream; **passive adopt** updates `chat_targets` in step 12 (D085). See § Passive epoch advance and § Peer reset.
3. **Contiguous tail** — `sender_seq == contiguous_peer_seq + 1` and `sender_seq > history_floor_seq[peer][epoch]`.
4. **Tail bootstrap** — per-epoch transcript empty; ingest tail batch without requiring seq 1..N first (only seq **> floor** when floor is set).
5. **Late fill (D067)** — `sender_seq` is in `empty_closed_seqs[]` for this `(peer, epoch)`, no row at that seq yet, and `message_id` not seen → accept; remove seq from `empty_closed_seqs` on persist.

**Gap (repair allowed; not compromised until repair fails or conflict):**

- `sender_seq > contiguous_peer_seq + 1` and `sender_seq > history_floor_seq[peer][epoch]` → request missing range via D058; on success, reclassify as normal.
- **`FetchChatTargetMessages` returns success with zero messages** for the requested gap range → **authoritative empty close** (D061) **only if D067 guard passes:** no local `relay_visible` row with `sender_seq > gap_max` for that peer/epoch. On close: advance `contiguous_peer_seq` across the range; append closed seq values to `empty_closed_seqs[]`. **If guard fails** (higher seq already held — e.g. peer received **6** while **5** still in sender's outbox): keep `sync_state=gap`; wait for live delivery of missing seq or transport exhaustion — do **not** empty-close.
- If repair returns **conflicting** seq (`message_id` mismatch) or impossible ranges → **soft compromised** (D038 choice sheet). Below-floor rows in a response are silently discarded (D037), not a compromise trigger.
- **Transport / 5xx failures** count toward **`kMaxGapRepairRounds`** (D041); empty authoritative success does **not**.

**Soft compromised (pause + user choice — D038):**

| Condition | Why |
|-----------|-----|
| Same `(peer, epoch, sender_seq)` + **different** `message_id` | Seq conflict (D011) |
| `sender_seq < contiguous_peer_seq` and not benign duplicate and not **late fill** (D067) | Rewind within epoch |
| `sender_seq = 1` in an **established** epoch where `contiguous_peer_seq > 0` | Sender reset without epoch bump |
| Gap repair exhausted (**transport** failures only, D041) or returns violating messages | Repair failed |

On soft compromised: **pause ingest and outbound**, set `sync_state=compromised`, append **integrity incident**, show choice sheet. Do not persist the triggering inbound row until user resolves (except record incident metadata). Recovery: see § Integrity recovery.

**Hard reject:**

| Condition | Why |
|-----------|-----|
| `envelope.session_epoch < chat_targets.session_epoch` | Illegal rollback (epoch decrease) |
| Invalid signature / AEAD decrypt failure / wrong thread | Wire or crypto invalid |
| **`rotate_psk`** traffic before local new PSK installed | Decrypt cannot succeed (D085) |
| `sender_contact_id` not a participant | Authorization |

Reject message permanently. Pause ingest/outbound if not already paused; show incident with **Pause only** or recommended recovery.

### Integrity recovery (D038, D089)

**Private direct (`e2e`)** uses strict seq compromise UX. **`e2e_public`** and **`group`** use relaxed ingest by default (D046).

#### `[v1]` recovery — private direct only

On soft compromised: pause → choice sheet (what, causes, risk) → user **must** pick:

| Option | Action |
|--------|--------|
| **Start new secure chat** | `rotate_psk`, epoch bump transaction (retired PSK ledger — E018/D083) |
| **Pause only** | remain paused |

No **continue anyway** in v1 (D046).

**Disclosure (Start new secure chat):** Messages already saved on this device stay readable. Encrypted copies on the relay from before rotation can still be unlocked on this device; new messages use the new key. Other devices need the new key for the new epoch.

**E2E new secure chat flow** (`rotate_psk`):

Local state machine: `ok` → `gap` → `compromised` → (`awaiting_new_psk` internal UI/coordinator sub-state) → `ok`.

```
Initiating side                               Innocent peer
     |                                              |
     | 1. User confirms new secure chat             | 1. May see peer pause banner
     | 2. Export PSK bundle OOB (E020)           | 2. Import bundle; active_epoch + retired tail
     | 3. Epoch bump transaction (§ above)          |    (D086 — sets session_epoch at import)
     | 4. Resume at seq 1+                          | 3. First ingest at active_epoch (normal D013)
     |                                              | 4. Fresh watermarks for new epoch
```

- Innocent peer accepts **strictly higher** `session_epoch` (D014, D085) on **first successful ingest** after decrypt when **epoch-only** — **passive adopt** txn in step 12 updates `chat_targets` + `threads` denorm together with the message row.
- **`rotate_psk`:** innocent peer **imports rich OOB bundle** (E020/D086) **before** epoch-`N` traffic can decrypt — bundle sets `master_psk_b64`, merges `retired_psks_json`, `session_epoch = active_epoch`, resets `next_outgoing_seq`, cancels old-epoch pending. First ingest at `active_epoch` uses normal D013 (epochs already aligned).
- **Epoch-only (D014):** same `master_psk`; passive adopt on first ingest — no OOB bundle; HKDF uses `envelope.session_epoch` directly.
- **Multi-hop `rotate_psk` (O006):** bundle carries up to **`kMaxRetiredPskEpochs` (8)** retired keys — relay ciphertext outside the tail may not decrypt on this device (disclose on import).
- **No `epoch_start` system row** — first user message may use `sender_seq=1`.
- **Historical decrypt (E018/D083):** `retired_psks_json` holds previous `master_psk` per epoch after `rotate_psk`; epoch-only bump re-derives from current PSK. Retired keys decrypt historical relay ciphertext only — **no new ingest on old epoch** after rotation.

#### Compromised thread behavior (D068)

While `sync_state=compromised` (including after **Pause only**):

| Subsystem | Behavior |
|-----------|----------|
| **Inbound ingest** | Paused — do not persist new peer rows (except incident metadata). |
| **Outbound / outbox** | **Frozen** — no background retry, no manual **Retry send**; show why next to pending/failed rows. |
| **Gap repair / tail sync / Sync with peer** | **Disabled** — integrity choice sheet is the only recovery path besides delete thread. |
| **User resolution** | **`rotate_psk`** → epoch bump transaction (cancels old-epoch pending per § Epoch bump). **`pause_only`** → remain frozen until rotate or delete. |

Copy must not tell users to **Retry send** while compromised — point to **Start new secure chat** or **Delete conversation**.

#### Relaxed ingest / continue anyway — public direct and group (D046)

**Default for `e2e_public` and `group`** when those tiers ship (with auto-key for public direct — not optional). Extends `sync_state.state_json`:

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
| **Receiver** | **Before** `DELETE FROM messages`: set `history_floor_seq[peer][epoch]` to **`loaded_max_seq[peer][epoch]`** (max peer `sender_seq` present in the transcript — includes gap-repaired rows, not contiguous alone); purge `profile.db` `outbox` for this thread; `UPDATE threads` set `preview=''`, `unread_count=0`; then delete messages; reset `loaded_min`/`loaded_max`/`contiguous_peer_seq` watermarks (empty transcript) |
| **Below floor** | `sender_seq ≤ floor` in same epoch → **silent discard** on all paths (poll, direct, tail, gap). No persist, backfill, show, or unread bump. |
| **Above floor** | Normal D013 ingest — tail, gap repair; **`[post-v1]`** authorized history backfill in `(floor, loaded_min)` |

The sender does not need a signal that the peer cleared locally; honest senders continue forward. Per-thread dedup (D034) is wiped with the transcript; seq floor — not a message-id registry — defines the sync boundary after clear. Full restart requires **epoch bump** (D014), not clear.

### Peer reset / new device (fresh stream)

When a peer wipes local state, installs on a new device without backup, or explicitly starts over:

1. **Bump `session_epoch`** via epoch bump transaction (D014).
2. Reset `next_outgoing_seq = 1` for the new epoch.
3. **No `epoch_start` system message** — first relay-visible user content may use `sender_seq=1`.
4. **Receiver** on unseen higher epoch: fresh per-epoch watermarks; `sender_seq=1` is normal bootstrap; **passive adopt** (D085) aligns local `chat_targets.session_epoch` and `next_outgoing_seq` on first successful ingest.

**Restored backup** (same `profile.db` + crypto sessions): not a reset — continue same epoch and seq.

### Passive epoch advance (D085)

When the **peer** bumps `session_epoch` first, the innocent device must not remain on a stale local epoch while decrypting and classifying inbound traffic at the peer's epoch. **Decrypt and HKDF always use `envelope.session_epoch`** (E019); **`chat_targets.session_epoch` is the outbound authoritative counter** and may lag until adopt.

**Trigger:** first E2E ingest where `envelope.session_epoch > chat_targets.session_epoch`, decrypt succeeds (step 7), and D013 accepts (classifier rule 2).

**Same dual-DB transaction as step 12 persist** (atomic with the inbound message row):

| Action | Epoch-only (D014) | `rotate_psk` |
|--------|-------------------|--------------|
| `chat_targets.session_epoch` | `= envelope.session_epoch` | `= envelope.session_epoch` |
| `next_outgoing_seq` | `= 1` | `= 1` |
| `retired_psks_json` | no change | merged from **E020 bundle** at OOB import (D086); passive ingest append only if bundle omitted |
| `master_psk_b64` | unchanged | set from bundle `master_psk_b64` at import |
| Old-epoch pending / outbox | cancel (D068) | cancel (D068) |
| `threads.session_epoch` denorm | update | update |
| `sync_state` | fresh watermarks for envelope epoch | fresh watermarks; clear compromise if recovering |

**Outbound after adopt:** next send reads updated `chat_targets.session_epoch` and assigns `sender_seq` from `next_outgoing_seq = 1` in the adopted epoch — no window where decrypt used epoch 2 but outbound still stamps epoch 1.

**`rotate_psk` before OOB:** AEAD decrypt fails at step 7 → hard reject; user must import **PSK bundle** (E020) or raw key for initial setup (E011).

**Rich OOB bundle (D086/E020):** see [e2e-message-crypto DESIGN § Rich OOB PSK bundle](../e2e-message-crypto/DESIGN.md#rich-oob-psk-bundle-e020d086). Import sets `session_epoch` immediately; passive adopt (epoch jump) applies only for **epoch-only** peer bumps without bundle.

**No `sessions.json` sidecar** — all durable epoch/seq/PSK state lives in **`profile.db` → `chat_targets`** (D084).

Sending `sender_seq=1` without bumping epoch in an established epoch is **soft compromised** (D038 choice sheet; recommended path is epoch bump).

## Resource & trust bounds (D029–D033)

Canonical limits in [DECISIONS.md](DECISIONS.md) D029. Summary:

| Area | Policy |
|------|--------|
| Compose / send | Reject empty and > 64 KiB `text`; validate `ChatPayload` before send |
| Wire | Max 256 KiB envelope JSON; **no remote `content_rml`** (D030) |
| Storage | Size checks on insert; LRU of open `thread.db` (max 16); `user_payload` ≤ 64 KiB (D029) |
| UI | `GetMessagesPage` by `display_order` (D054); default 100 rows (D031) |
| Agent context | `GetMessagesForContext` tail + summary — no full-thread load (D039) |
| Poll | Min **2 s** interval while foreground (D032); max 100 messages per batch |
| Outbox | `profile.db`-backed; max 500 pending retry items; **12** attempts per message (D041) |
| Gap repair | Max **5** rounds, **500** seq span per fetch (D041) |
| Integrity incidents | Max **10** per `(peer, epoch)` ring buffer (D049) |
| Empty closed seqs | Max **128** singleton entries; coalesce to `empty_closed_ranges[]` (D071) |
| Retired PSK ledger | Max **8** epochs in `retired_psks_json` and OOB bundle tail (D086/E020) |
| PSK bundle OOB | Max **4 KiB** serialized JSON (E020) |
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
virtual Roe<Thread> FindOrCreateDirectThread(
    const ChatTargetKey& target,
    const std::string& participant_contact_id) = 0;

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

// D031 — UI transcript window (newest page first; scroll-up passes oldest loaded display_order)
virtual Roe<std::vector<ThreadMessage>> GetMessagesPage(
    const std::string& thread_id,
    std::optional<int64_t> before_display_order,
    size_t limit = 100) const = 0;

// D039 — agent / LLM context (tail + optional summary; no full-thread scan)
virtual Roe<std::vector<ThreadMessage>> GetMessagesForContext(
    const std::string& thread_id, const ContextBudget& budget) const = 0;
```

Send path: reject compose text and serialized payload over D029 limits before `AppendMessage`.

Keep `DeleteThread`, `AppendMessage`, `UpdateMessage`. Change `HasMessageId` to **`HasMessageId(thread_id, message_id)`** — per-thread `messages.id` lookup (D034); drop profile-global dedup from `JsonThreadStore` cutover.

`GetMessages(thread_id)` — **tests and export only**; feature code uses `GetMessagesPage` (UI) or `GetMessagesForContext` (agent). **v2a cutover:** grep gate — no `GetMessages` / profile-global `HasMessageId` in `src/feature/` (D057).

### SQLite operations (D044)

- WAL mode; one writer mutex per `thread.db` and `profile.db`.
- **Lock order** when both files are touched: acquire **`profile.db` mutex first**, then `thread.db` (same order in epoch bump, `AppendMessage` catalog update, delete). Never hold `thread.db` while waiting on `profile.db`.
- **Dual-DB write recipe** (`AppendMessage`, `ClearMessages`, `DeleteThread` when both DBs touched):

```
lock(profile_mutex)
lock(thread_mutex)
  BEGIN thread.db
    write messages (+ display_order, sync_state on clear)
  COMMIT thread.db
  BEGIN profile.db
    UPDATE threads / INSERT|DELETE outbox / chat_targets as needed
  COMMIT profile.db
unlock(thread_mutex)
unlock(profile_mutex)
```

Write **authority** remains `thread.db` first inside the critical section. Crash between commits: message without catalog row → D035 list verify; orphan outbox row → startup reconciliation (D047).

- `ClearMessages` → compute floor + purge outbox + catalog preview/unread (D037, D024) → `DELETE FROM messages` → `wal_checkpoint(PASSIVE)`.
- `AppendMessage` / outbox insert: set `outbox.updated_at` for startup ordering (D041).
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
- **Gap banner** `[v1]` (E2E) — `sync_state=gap`; **Retry sync** (D059) then tap-to-repair.
- **Sync with peer** `[v1]` (E2E) — thread menu; **`FetchChatTargetMessages`** (D059).
- **Integrity banner** `[v1]` (E2E) — `sync_state=compromised`; choice sheet per § Integrity recovery; outbox retry disabled (D068).
- **`@ai`** `[v1]` local; `[post-v1]` shared modes — § `@ai` in direct threads.
- **Clear history (D024, D057)** `[v1]` — choice sheet → **confirmation dialog** with pre-clear inventory (messages, gap-repaired rows, pending/failed sends, optional forget-AI) → clear or cancel.
- **Forget AI memory** `[v1]` — separate action (`memory` table only).

## Non-goals

Not planned (distinct from **`[post-v1]`** items above, which *are* specified):

- **Multi-device concurrent send** on **private direct** (D015 — single active device on `e2e`; target support on `e2e_public` / group — D089)
- **Group chat** — `[post-v1]`; pairwise E2E (E022) before ingest ships
- **Legacy on-disk migration** from pre-D028 JSON (D016)
- Full-text search UI (SQLite FTS **`[future]`**)
- **Plaintext `public_relay` direct wire** (D090)
- Retraction / “unsend” on relay
- SQLCipher / transcript encryption at rest (D048 — wire-only E2E confidentiality in v1)

## Success criteria

### `[v1]`

- [ ] All AI sidebar threads persist via `IThreadStore` (no orphan `Conversation`-only path).
- [ ] Clear history / forget memory / delete conversation per D024.
- [ ] Same local Contact may have separate **private** and **public direct** thread shells per **communicating identity**; tier badge in sidebar (D004, D079, D089). **Functional `e2e_public` messaging** (auto-key, relaxed ingest) is **`[post-v1]`** — see § Three chat tiers phasing.
- [ ] Message IDs stable; relay dedup on all tiers.
- [ ] **Private direct (`e2e`):** `sender_seq`, tail + gap sync, **user-initiated sync** (D059), strict integrity UX (rotate or pause only).
- [ ] **`FetchChatTargetMessages`** peer-first + relay fallback (D058/D060); authoritative empty gap close with D067 guard + late fill.
- [ ] Compromised **private** thread freezes outbox and sync (D068); epoch bump cancels old-epoch pending.
- [ ] Direct wire: **`e2e`** + **`e2e_public` only**; reject `public_relay` (D090); no `thread_id` (D056).
- [ ] `ChatTargetKey` ingest routing + `display_order` pagination (D054, D056).
- [ ] Durable outbox + `chat_targets` in `profile.db` + reconciliation (D047).
- [ ] Clear history floor (D037); epoch bump transaction (D014).
- [ ] Local `@ai` only; ChatPayload `text` + `system`; D029–D032 bounds.
- [ ] `GetMessagesForContext` hot path; async compaction (D040).
- [ ] Relay fetch + ingest chat-target authorization (D027, D056).

### `[post-v1]` / mature (enable when phased)

- [ ] Rich ChatPayload types render; annotation cap + orphan UX (D042–D043).
- [ ] Shared `@ai+` / `@ai++` with correct seq on trigger user stream.
- [ ] **`e2e_public`:** auto-key, encrypted bodies, relaxed ingest default (D046).
- [ ] **Group:** pairwise sender-keys E2E (E022); membership + ingest.
- [ ] Scroll-driven history backfill via D027.
- [ ] Per-message transport badge when libp2p direct exists.
- [ ] Optional sidebar grouped sections.
