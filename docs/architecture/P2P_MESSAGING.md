# P2P messaging

**Tier:** architecture

Person-to-person chat in pp-browser uses a **foundation-first** architecture: one `ThreadMessage` model for AI sessions, direct, and group threads; local persistence as source of truth; HTTP relay/directory/registration transport against Brief by default (`https://www.brief.global/api/relay`).

**Normative wire shapes:** [WIRE_SCHEMAS.md](../contracts/WIRE_SCHEMAS.md). **E2E crypto:** [MESSAGE_ENCRYPTION.md](../contracts/MESSAGE_ENCRYPTION.md). **Compatibility:** [COMPATIBILITY.md](../contracts/COMPATIBILITY.md).

## Service resolution

[`CreateOrgBackendClients`](../../src/base/net/OrgBackendClientFactory.cpp) picks an implementation per endpoint:

1. `base_url` set (platform default or config) → HTTP client (`HttpRelayClient`, etc.)
2. else client left unset (should not happen after defaults / empty coalesce)

Native messaging code (`MeshDeliveryOrchestrator`, `MessagingTools`) always calls `IRelayClient` / `IDirectoryClient` / `IRegistrationClient`; the factory swaps implementations underneath. See [SERVICE_ENDPOINTS.md](../contracts/SERVICE_ENDPOINTS.md).

**Relay inbox (delivery queue):** Offline ingest uses signed `POST …/v1/inbox/poll`. Non-empty pages always return `next_cursor`; the client persists that watermark under the profile (`relay_inbox_cursor.json`) and must not clear it on empty polls. After ingest, the client still calls `POST …/v1/inbox/ack` (soft-ack **M013**: validates cursor, **does not delete** shared mailbox rows so sibling devices under one `relay:` are not starved). Messenger TTL-expires rows after **90 days** and also applies a **soft per-recipient FIFO cap** (trim toward **1000** when a mailbox exceeds **~1200**; sampled on send). Me → Security offers **Clear undelivered older than 7 days** (`POST …/v1/inbox/clear`) for recovery — account-wide, unrelated to thread **Clear history** (`history_floor_seq`). Chat truth remains local SQLite + stream/P2P history sync.

**Relay history (D027):** `IRelayClient::FetchChatHistory` — `HttpRelayClient` uses signed `POST …/v1/streams/messages/query`. Unit tests may construct `MockRelayClient` directly. See [WIRE_SCHEMAS § Stream history](../contracts/WIRE_SCHEMAS.md#stream-history-http-relay). Live integration tests ([D093](../../projects/chat-storage-and-memory/DECISIONS.md#d093--relay-backend-for-v6-sync-d027)) run when these env vars are set:

| Variable | Purpose |
|----------|---------|
| `PP_BROWSER_RELAY_INTEGRATION_URL` | Relay base URL |
| `PP_BROWSER_RELAY_INTEGRATION_REQUESTER` | Requester `relay_user_id` |
| `PP_BROWSER_RELAY_INTEGRATION_PEER` | Peer `relay_user_id` for history query |
| `PP_BROWSER_RELAY_INTEGRATION_SIGN_KEY_HEX` | Ed25519 seed (hex) for relay API signing |

CI constructs `MockRelayClient` in unit tests; `pp_browser_relay_live_integration_test` skips when env is unset.

## Data model

### Thread

| Field | Description |
|-------|-------------|
| `id` | UUID; **local only** — not on wire (D056) |
| `kind` | `ai`, `direct`, `group` |
| `participant_contact_ids` | One peer for direct; N for group (future) |
| `unread_count` | Sidebar badge |
| `preview` | Last message snippet |

**Target (v2b+):** `channel` (`e2e` \| `e2e_public`) on direct threads — see [DESIGN § Three chat tiers](../../projects/chat-storage-and-memory/DESIGN.md#three-chat-tiers-d089).

### ThreadMessage

| Field | Description |
|-------|-------------|
| `sender_contact_id` | **Local rows:** `local:self`, `ai:assistant`. **Wire / peer rows:** communicating identity value (D079, D082), e.g. `relay:abc123` |
| `content_rml` | Rendered assistant blocks (optional; **local AI only** — never from wire, D030) |
| `relay_visible` | `false` for `@ai` assist (never relayed) |
| `delivery` | `local`, `pending`, `relayed`, `failed` |

Special IDs: `local:self`, `ai:assistant`.

## Persistence

Profile-scoped layout (see [DATA_LAYOUT.md](../contracts/DATA_LAYOUT.md)). **Legacy (today):**

```
{data_dir}/profiles/{profile_id}/
  identity.json
  contacts.json
  threads/index.json
  threads/{thread_id}.json
```

**Target (v2a+):** see [chat-storage-and-memory DESIGN.md](../../projects/chat-storage-and-memory/DESIGN.md) — `profile.db` (`threads` + `outbox` + `chat_targets`) + per-thread `thread.db`; no `index.json`.

Configure endpoints via user config (`~/.config/pp-browser/config.json` on Linux) or in-app **Me → Network**:

```json
{
  "data_dir": "~/.local/share/pp-browser",
  "relay": { "base_url": "https://www.brief.global/api/relay" },
  "directory": { "base_url": "https://www.brief.global/api/relay" },
  "registration": { "base_url": "https://www.brief.global/api/relay" },
  "libp2p": {
    "node_enabled": true,
    "mesh_enabled": true,
    "amp_udp_port": 0,
    "bootstrap_peers": [
      "/ip4/54.198.185.139/udp/443/adp/1.0.0/p2p/QmbgShE3J3G6fvEHqwTS5ooQiFnYn46rzn6cTexUGWdWaj",
      "/ip4/44.218.209.223/udp/443/adp/1.0.0/p2p/Qmd2m4skBPf7YjPaAKcnhPuhzRv6wEnsf9QheZRANYGuM7"
    ]
  }
}
```

**Libp2p roles (n1):** Desktop defaults to **Node** (`node_enabled`); Me → Network can opt out. Mobile is always **Client** (no Node hosting). Amp UDP accept remains on for Clients. Clients still dial the Brief seed from `bootstrap_peers`. See [p2p-mesh](../../projects/p2p-mesh/).
Platform defaults use the Brief URLs above. Empty `base_url` values coalesce to those defaults; production never falls back to in-process mocks (`Mock*Client` is test-only).

## Relay envelope (target — D056, D090)

**No `thread_id` on the wire.** All direct tiers use **`body.e2e.payload_b64`** (AEAD ciphertext). Reject `public_relay`, `body.content_b64`, flat `body.text`, and legacy `thread_id`.

Full spec: [WIRE_SCHEMAS § RelayEnvelope](../contracts/WIRE_SCHEMAS.md#relayenvelope-v1--envelope_version-1).

```json
{
  "envelope_version": 1,
  "message_id": "550e8400-e29b-41d4-a716-446655440000",
  "sender_relay_id": "relay:alice123",
  "sender_contact_id": "relay:alice123",
  "route": {
    "kind": "direct",
    "channel": "e2e"
  },
  "sender_seq": 42,
  "session_epoch": 1,
  "body": {
    "e2e": { "payload_b64": "…" }
  },
  "timestamp": 1719662400123,
  "signature": "base64-ed25519"
}
```

**Private direct:** `"channel": "e2e"`. **Public direct:** `"channel": "e2e_public"` (same body shape).

**AEAD plaintext** is binary **ChatPayload** (D087/D090) — not JSON `body.content`. See [MESSAGE_ENCRYPTION.md](../contracts/MESSAGE_ENCRYPTION.md).

Inbound routing: `ChatTargetKey { peer_identity_kind, peer_identity_value: sender_contact_id, channel }` → local thread lookup (**private `e2e`:** find-only, D062; **`e2e_public`:** auto-create after decrypt, D080).

**Signature verify (E014, E016):** Inbound messages are verified with **`EnvelopeSigner::Verify`** using the sender's Ed25519 public key from **`PeerSigningKeyStore`** (directory at add-contact; lazy `GET /v1/users/{relay_user_id}` on cache miss). See [e2e DECISIONS E016](../../projects/e2e-message-crypto/DECISIONS.md#e016--peer-signing-keys-relay-directory-source-local-cache-oob-fingerprint-at-add).

**Wire cutover (D063):** v2a-p2p ships this final envelope shape. v4 adds ChatPayload **validation** only — no second wire break. See [DESIGN § Wire cutover phasing](../../projects/chat-storage-and-memory/DESIGN.md#wire-cutover-phasing-d063).

**Private `e2e` encrypt:** outbound send and inbound poll decrypt via `E2eRelayPayloadCodec` (c2). **`e2e_public`** remains plaintext on wire until c3 auto-key.

Local store is written **before** send. Server rejections do not delete history. **Unsent/failed** rows stay local — user **retries send**; **peer sync** (`FetchChatTargetMessages`, D058) fetches **missing messages from the peer**, not your pending outbox.

## E2E history sync (v6 — D058–D060)

| Mode | Trigger |
|------|---------|
| Tail sync | Open E2E thread, reconnect (`TailSync` — fetches seq > `loaded_max_seq`) |
| Gap repair | Automatic on seq hole; **Retry sync** banner (D059) |
| User sync | Thread menu **Sync with peer** — tail + gap repair + one older-history page (D059) |
| Scroll backfill | **Load older messages** banner at transcript top (D052/post-v6c) |

**Transport:** libp2p peer-direct `/pp-browser/rpc/history/1.0.0` first; relay `POST …/v1/streams/messages/query` fallback (client maps `ChatHistoryRequest` → `stream_key` / `order_key`). Full spec: [WIRE_SCHEMAS § Stream history](../contracts/WIRE_SCHEMAS.md#stream-history-http-relay).

### Direct live send (`/pp-browser/rpc/chat/1.0.0`)

Outbound `SendUserMessage` tries libp2p direct first when the peer has a registered dialable multiaddr (`Contact.multiaddrs` / `RegisterPeerDirectEndpoint`); on failure falls back to `IRelayClient::Send`. Inbound direct envelopes use the same `RelayEnvelope` shape and `RelayReceivePipeline` with `MessageTransport::Direct`.

**Session policy** (`PeerSessionManager`): on-demand dial, reuse existing connections, warm the open-thread peer, idle disconnect, global connection caps. No separate app connection pool.

Per-message **Direct / Relay / Local** badges read the persisted `transport` column (post-v6d).

**On-wire framing & failure handling** (length-prefixed JSON on Noise+Yamux, shorter/longer/hang budgets): [LIBP2P_STREAMS.md](LIBP2P_STREAMS.md).

## Identity model

**Code today** (single-device): table below. **Multi-device target** (pre-release design freeze): Account ID = person; Peer ID = per-device endpoint; `relay:` = route binding — [multi-device-account](../../projects/multi-device-account/) ([D099](../../projects/chat-storage-and-memory/DECISIONS.md#d099--account-id-amends-d096-multi-device)).

| Role | Example | Use |
|------|---------|-----|
| **Who** (network id) | libp2p Peer ID | Me primary, dial/bind, direct-only threads when no relay |
| **Find** (lookup) | CAIP-10 `eip155:…:0x…`, nickname | Search / attest → resolve to Peer ID |
| **Route** (optional offline) | `relay:…` | Relay inbox + preferred v1 `ChatTargetKey` / wire when present |

**Local contact book:** `Contact.id` is a stable UUID (local only). `Contact.ids[]` holds external handles (`relay_user`, `peer_id`, …). Threads link via `participant_contact_ids` → `Contact.id`.

**Thread target from contact** (`DirectChatTargetFromContact`): prefer primary/first `relay_user`; else `peer_id`. Relay is **not** required. Direct-only messaging needs peer ID **and** at least one dialable multiaddr (`Contact.multiaddrs`); endpoints register under the same chat-target identity value.

See [D096](../../projects/chat-storage-and-memory/DECISIONS.md#d096--identity-roles-peer-id-who-caip-10-find-relay-route), [D091](../../projects/chat-storage-and-memory/DECISIONS.md#d091--blockchain-contact-id-caip-10-e024).

## Messaging UX

- **Me tab** — nickname, **Peer ID** (primary), Relay ID (secondary / after register), Copy ID / Share (Peer ID), Register / Rotate Brief API key; preference rows (Assistant, Network, …) stay one tap away.
- Sessions header **`+`** opens a menu:
  - **Chat with AI** — new AI thread
  - **Message a contact** — switch to Contacts to pick a peer (then Secure / Public on contact detail)
  - **Find someone** — new AI thread with draft prefilled for directory discovery
- Home is a prompt-first landing (no sticky AI thread). First send/suggestion creates a new AI session and switches to Sessions. Home chips are AI intents (find someone, headlines, articles, get started, capabilities); messaging entry points live on Sessions / Contacts.
- **Contacts tab** header **`+`** opens a menu:
  - **Add contact** — create an empty contact (`ContactsStore::AddEmpty`), open detail (**You**: display name + trust; **Directory**: nickname / relay / peer / multiaddrs as meta text; **Sync from directory…** when a relay id is present, refreshes remote only)
  - Directory add merges by `relay_user` id (`AddFromDirectoryHit` updates remote, preserves local annotations)
  - `contacts.json` shape: `{ schema_version: 1, contacts: [{ local, remote, overrides{} }] }` — unversioned/flat legacy migrates on load
  - **Find someone** — same as Sessions find flow
- Secure / Public on contact detail are enabled when the contact is **routable** (relay ID, or peer ID + multiaddr). Otherwise a short hint is shown.
- **Peer link status** (direct threads) — chat header shows live link state (`Connecting…` / `Direct` / `Via relay` / backoff countdown). Soft banner + **Retry connection** on dial backoff; toast when a send falls back to relay after a direct attempt.
- Directory discovery still uses agent tools: `search_people`, `list_contacts`, `list_conversations`, `open_conversation`, `start_conversation`.
- Results render as `long_list` blocks with identity-first rows (title / ~nick · @shortId / optional “In contacts”) and primary **Add contact** or **Message** chips (`send_chat_action` + JSON `payload`).
- **Registration** also via `register_user` / `update_profile_nickname` tools (alternate to Me tab).

## @ai in direct threads

Composer: `Message… or @ai ask assistant` (max length `kMaxComposeTextBytes`).

| Mode | Prefix | Relay |
|------|--------|-------|
| Local assist | `@ai …` | No — `relay_visible=false`, local `ai:assistant` row |
| Shared reply | `@ai+ …` | Yes — user prompt relayed; AI reply sent with `generation=ai_on_behalf` |
| Shared full | `@ai++ …` | Yes — user prompt + full AI reply relayed to peer |

Shared modes show a one-time confirm dialog before first send. Parser: `AtAiParser` (`@ai share`, `@ai share all` aliases).

Local `@ai` uses `AgentSession::SubmitScopedAssist` with thread transcript context.

## Modules

| Path | Role |
|------|------|
| `src/feature/conversations/ConversationsHub.*` | App messaging assembler (`ConversationsCore`): stores/inbox/P2P; owns `MeshHost`; calls and broadcast ride it as `HubParticipant`s (`feature/product/ProductStack`) |
| pp-cpp-mesh `MeshHost.*` | Shared mesh host (NodeRuntime + dial-back + circuit/media relay + reachability); also used by `pp-node` |
| `src/feature/calls/CallStack.*` | Call media / CSM / lifecycle / bridge (app-only) |
| `src/feature/conversations/ConversationsFacade.*` | UI/tools façade over Hub (no direct accessor peeks) |
| `src/feature/conversations/InboxController.*` | Active thread, unread, the transcript window (paging) and the facts drawing it needs (names, control lines, attachment state) |
| `src/domain/messaging/TranscriptAssembly.*` | Which rows a transcript shows (never lane signaling, never what hung off a deleted message), reactions / badges folded onto their targets, unlinked annotations, the asked question on an AI answer |
| `src/gui/chat/TranscriptRml.*` | The transcript drawn as bound rows (bubbles, cards, system lines, chips); every click argument escaped for its attribute |
| `src/feature/conversations/MeshDeliveryOrchestrator.*` | Send (direct→relay), poll, dedup, sync UX |
| `src/domain/messaging/ChatSyncLogic.*` | Chat-history sync rules over the peer watermarks: what a tail sync / gap repair / older backfill asks for, the floor and batch bounds, closing an empty gap, extending the contiguous run |
| `src/domain/messaging/DeliveryNotice.*` | What a failed send, a relay fallback, a partial group send or an unreadable message tells the user — as kinds (`SendFailure`, `ReceiveFailure`); the only place transport error text is read. The GUI words them (`gui/chat/DeliveryNoticeText`); a local receive-failure line stores its reason, not words |
| `src/domain/messaging/OutboundMessage.*` | What a send of ours writes and puts on the wire: the outbound row (sender seq, epoch, reaction cap), the direct send gate, the sealed payload (master PSK, or a public session opened to the peer's account key with its key_init), the direct / group-member envelope, Amp-first vs relay-first |
| `src/feature/conversations/Libp2pChatHistoryService.*` | D060 history over shared host |
| `src/feature/conversations/Libp2pDirectChatService.*` | `/pp-browser/rpc/chat/1.0.0` push |
| `src/feature/conversations/ChatSyncWorkflow.*` | `FetchChatTargetMessages`, tail/gap/user sync (D058–D059) |
| `src/domain/messaging/RelayReceivePipeline.*` | Inbound verify + classifier + backfill ingest (the local keys from people through `LocalKeys`) |
| `src/feature/conversations/MessageRouter.*` | Composer routing |
| `src/feature/conversations/ContactActionDispatcher.*` | Chip payloads |
| `src/feature/conversations/MessagingTools.*` | Agent tool definitions |
| `src/base/people/ContactsStore.*` | Local contacts.json; `AddEmpty` / `AddFromDirectoryHit` (merge) / `ApplyRemoteSnapshot` / `Upsert` |
| `src/gui/contacts/ContactsController.*` | Contacts list/detail UI; local edit + Sync; message gating |
| `src/base/messaging/DirectChatTarget.*` | Contact → `ChatTargetKey` identity (relay preferred, peer fallback) |

## System-control families

A feature's control messages — call control, initiation billing, payment promises — are **system
messages** whose payload names a `control_type`. Messaging does not know what they mean: a feature
binds a `SystemControlFamily` (`common/chat/SystemControl.h`) in the `SystemControlRegistry` the hub
owns, and messaging routes by what the family claims. A family's messages travel one of two ways:

- **The chat stream** (default): end to end, sequenced, stored — history the user reads (a call
  summary, a payment receipt).
- **The control lane** (`on_lane`): signaling with no place in a transcript — see below.

| Field | Messaging uses it for |
|-------|-----------------------|
| `owns` | Which system messages are the family's |
| `apply` | Side effects of an inbound one; an error refuses the message (`couldn't apply <family> update`) |
| `on_lane` | Which of its messages ride the control lane (one arriving in a chat stream is dropped) |
| `lane_max_age_ms` | How long a lane message may have waited in the relay (relay clock) and still apply |
| `quiet` | A stream message shown, but with no unread count, preview or notice |
| `line` | A stream message's transcript line, names included (`SystemControlLabels`); none: its text |
| `live` | A live exchange (a call): messaging holds key rotation and defers background history sync |

Bound today:

| Family | Bound by | Notes |
|--------|----------|-------|
| `call` | the call stack (`domain/calls/CallControlFamily`) | All signaling on the lane; one `call_summary` per call in the chat stream, worded from the reader's side (quiet unless it is a call we missed) |
| `billing` | the hub (`PricingControlFamilies`) | Chat stream. Keyed on the authenticated sender; a message naming another peer is refused |
| `payment promise` | the hub (`PricingControlFamilies`) | Chat stream. Staged only from our counterparty on the promise, signed by it, and a legal change of our copy (`MergeInboundPaymentPromise`: new offer; accept of our offer; outcome by it as actor; terms never change). The user's accept re-checks and commits |

The receive pipeline, delivery and the inbox read the registry; membership updates and PSK rotation
are messaging's own and stay in it. Families bind and unbind from any thread (the call stack rebuilds
itself); readers take an immutable snapshot.

### The control lane

Signaling between a pair (call invite, accept, media keys, hop moves, roster …) travels outside the
ordered chat stream:

- **Wire:** route kind `control` (`kRelayRouteControl`, signed like any route kind), unsequenced
  (`sender_seq` 0, bound into the AEAD's AAD), E2E under the pair's **e2e_public session** — the
  account pair's AutoKey session, the one media keys are wrapped under; no thread. Its own relay
  stream (`v1:control:…`), so it never shows up as a gap in the chat stream or in history sync.
  Built by `BuildControlLaneEnvelope`; sent by `MeshDeliveryOrchestrator::SendControl` on the
  critical worker lane — relay when a route is known (retried for two minutes), else Amp.
- **Receive** (`RelayReceivePipeline::ProcessControlEnvelope`): signature, decrypt, a bound family
  that sends this message on the lane, **fresh** (sender's signed time within an hour of ours;
  through the relay, waited no longer than the family's `lane_max_age_ms` on the relay's clock — no
  skew between devices), **first seen** (`ControlLaneSeenStore`, `control_lane_seen` in profile.db,
  per sender and id, kept three hours), then the family's `apply`. Never stored, never a transcript
  row, no inbox chrome.

There is no control thread: no hidden e2e_public twin beside a private DM, no pruning, no
out-of-order carve-outs in the chat stream.

## A/V calls (signaling over messaging)

Call signaling rides the control lane; each call leaves one `call_summary` (outcome, media,
duration) in its thread's chat stream, written by its starter. Media is a separate plane
(PeerConnection or `media_relay`). Code ownership and target split: [CALLS.md](CALLS.md). Product
ADRs: [projects/p2p-av-calls/](../../projects/p2p-av-calls/).

## Group chat (future)

`ThreadKind::Group` and `participant_contact_ids[]` are reserved. Wire shape open (O008). Adding groups does not require a new local message schema.
