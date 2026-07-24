# Wire schemas — normative wire reference

**Tier:** contract

**Status:** Normative contract (promoted from chat-storage). Implemented for v2a-p2p+.  
**Authority:** This file is the **canonical wire reference** for implementers, relay API, and libp2p history (D072). Field-level product rules and rationale live in [chat-storage DESIGN.md](../../projects/chat-storage-and-memory/DESIGN.md) and ADRs. C++ codecs in `base/messaging` must match these types.

**Related:** [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md) (AAD, ciphertext, signing), [P2P_MESSAGING.md](../architecture/P2P_MESSAGING.md), [COMPATIBILITY.md](COMPATIBILITY.md), [e2e DESIGN](../../projects/e2e-message-crypto/DESIGN.md) (planning), [chat-storage CURRENT_STATE](../../projects/chat-storage-and-memory/CURRENT_STATE.md).

**C++ reference:** [`src/common/Serialize.hpp`](../../src/common/Serialize.hpp) (`WireLenUtf8`, `WireLenBytes`, `OutputArchive` / `InputArchive`); [`src/common/BinaryPack.hpp`](../../src/common/BinaryPack.hpp) (`binaryPack` / `binaryUnpack`).

**Three tiers (D089/D090):** Direct chat uses **`e2e`** and **`e2e_public` only** — both E2E via **`body.e2e.payload_b64`**. Reject **`public_relay`** and **`body.content_b64`**.

**Identity (D079, D082):** Wire **`sender_contact_id`** carries the sender's **communicating identity value** (e.g. `relay:abc123`) — not local `Contact.id`. **`Contact.id`** is address-book only. v1 relay: **`peer_identity_kind` = `relay_user`**, **`peer_identity_value` = `relay:` + opaque id** (relay-assigned, URL-safe; see [DECISIONS D082](../../projects/chat-storage-and-memory/DECISIONS.md#d082--relay-user-communicating-identity-string-format)).

---

## pp Binary Wire Profile (D088)

All in-tree **binary** wire formats (ChatPayload, AAD, E014 string fields, E2E blob tail) use this profile. **Do not** use bare `nlohmann::json::dump()` or ad hoc length widths on wire paths.

| Type | Encoding |
|------|----------|
| **LenUtf8** | `u64` BE byte count + UTF-8 bytes (no NUL). C++: **`WireLenUtf8`**. |
| **LenBytes** | `u64` BE byte count + opaque bytes. C++: **`WireLenBytes`**. |
| **Fixed integer** | Fixed-width **big-endian** (`u8`, `u16`, `u32`, `u64`, `i64`). |
| **Fixed raw** | `N` bytes with **no** length prefix (e.g. 32-byte hash, 24-byte nonce). |
| **Forbidden on wire** | `map`, `set`, `float`, pointers, JSON inside signed/hashed bytes |

**Decode rules:**

1. Reject unknown **`version`** bytes at format start (per-type policy below).
2. Enforce **application max size** before allocation ([D029](../../projects/chat-storage-and-memory/DECISIONS.md#d029--chat-resource-bounds-size--volume) — not the archive's internal 64 MiB ceiling).
3. **Exact consume:** decoded byte length MUST match input; **reject trailing bytes**.

**Encode rules (canonical):**

- Field order matches struct `serialize()` order in spec tables.
- ChatPayload **`text`** with default plain format: **omit** `sub_version` / `format` tail (Vectors A/B).

---

## Unknown-field policy (D073)

Forward-compat policy for older clients vs newer peers/APIs: [COMPATIBILITY.md](COMPATIBILITY.md).

| Layer | Rule |
|-------|------|
| **`RelayEnvelope`** (application blob) | **Ignore** unknown top-level keys on ingest after required fields parse. Required keys must be present. Do not fail on forward-compatible extensions. |
| **`RelayWireRecord`** (HTTP relay) | **Reject** unknown top-level keys. Relay is format-blind — only routing + `blob_b64`. |
| **`ChatPayload` (binary)** | **Reject** unknown `content_type` on **relay ingest** (D030/D050) today; target soft-skip / placeholder per [COMPATIBILITY.md](COMPATIBILITY.md). **Reject** unknown tail fields for known types in v1. |
| **`ChatHistoryRequest` / `ChatHistoryResponse`** | **Reject** unknown top-level keys (server/client negotiated API). |
| **Signature input** | Only documented canonical fields participate in signed bytes — unknown envelope keys are **not** signed unless a future `envelope_version` spec says otherwise. **Normative byte layout:** [e2e DESIGN § Ed25519 signing](../../projects/e2e-message-crypto/DESIGN.md#ed25519-canonical-signing-bytes) (E014). |

---

## `RelayWireRecord` (HTTP relay — opaque store-and-forward)

The relay **never parses** application envelopes. pp-browser wraps the signed `RelayEnvelope` (below) into `blob_b64` and sends routing metadata separately.

**Send** `POST /api/relay/v1/messages`:

```json
{
  "sender_contact_id": "relay:alice123",
  "recipient_contact_id": "relay:bob456",
  "stream_id": "v1:e2e:1:relay:alice123:relay:bob456",
  "index_key": 42,
  "blob_b64": "base64-of-application-envelope-json"
}
```

**Poll / history** returns `RelayInboundRecord[]` (no `recipient_contact_id`):

```json
{
  "sender_contact_id": "relay:alice123",
  "stream_id": "v1:e2e:1:relay:alice123:relay:bob456",
  "index_key": 42,
  "blob_b64": "…"
}
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `sender_contact_id` | string | yes | Sender relay id |
| `recipient_contact_id` | string | send only | Inbox owner |
| `stream_id` | string | yes | Opaque conversation scope (pp-browser: canonical `v1:…` stream key) |
| `index_key` | integer (u64) | yes | Monotonic per-sender per-stream; **v1:** equals `sender_seq` |
| `blob_b64` | string | yes | RFC 4648 base64 of application envelope JSON (unsigned routing fields omitted) |

**Dedup:** relay unique key `(sender_contact_id, stream_id, index_key)`. **Signature verification:** client-side only (`RelayReceivePipeline`).

**Codec:** `RelayWireSendRecordFromEnvelope` / `RelayEnvelopeFromInboundRecord` in `base/messaging/MessagingJson.*`.

---

## `RelayEnvelope` (v1 — `envelope_version: 1`)

Signed application wrapper **inside `blob_b64`**. **No `thread_id`.** See [DESIGN § Relay envelope](../../projects/chat-storage-and-memory/DESIGN.md#relay--direct-envelope-d056-d063).

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

**Public direct:** same shape with `"channel": "e2e_public"`.

### Ed25519 canonical signing (E014)

Implement via **`EnvelopeSigner`** in `base/messaging`. **Do not** sign JSON `dump()` of the envelope. String fields in sign bytes use **LenUtf8** (D088).

| Topic | v1 rule |
|-------|---------|
| Sign bytes | Fixed binary: domain prefix + `sign_version` + fields — full layout in [e2e DESIGN § Ed25519 signing](../../projects/e2e-message-crypto/DESIGN.md#ed25519-canonical-signing-bytes) |
| Signed fields | `envelope_version`, `message_id`, `sender_contact_id`, `route` (as enums), `timestamp`, `body_hash`, `sender_seq`, `session_epoch` |
| Direct channel enum | `0` = `e2e`, `1` = `e2e_public` (D090) |
| `body_hash` | BLAKE2b-256(`0x02` \|\| decoded E2E blob bytes) only — no `0x01` public path |
| Not signed | `thread_id`, `sender_relay_id`, `signature`, unknown keys |
| `signature` encoding | Standard **base64** (RFC 4648, padded) only in v1 |

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `envelope_version` | integer | yes | **1** in v1. Signed (D072). Bump independently of `ChatPayload.payload_version`. |
| `message_id` | string (UUID) | yes | Dedup key (D034) |
| `sender_relay_id` | string | yes | Relay registration id; **v1:** same string as `sender_contact_id` (D082) |
| `sender_contact_id` | string | yes | Sender **communicating identity value** (D079) — e.g. `relay:abc123` (D082) |
| `route` | object | yes | See `Route` below |
| `body` | object | yes | **`body.e2e`** — see below (D090) |
| `sender_seq` | integer (u64) | yes on direct | Both tiers (D045) |
| `session_epoch` | integer (u32) | yes on direct | |
| `timestamp` | integer (i64) | yes | Unix **milliseconds**; display metadata; not sort authority (D054). Included in sign bytes (E014). |
| `signature` | string | yes | Ed25519 over [canonical sign bytes](../../projects/e2e-message-crypto/DESIGN.md#ed25519-canonical-signing-bytes); **base64** (RFC 4648, padded) in v1 |
| `sender_instance_id` | string (UUID) | **`[future]`** | Multi-device extension (D074); omit in v1 |

### Relay routing metadata (unsigned — v1)

Set by the **sender client** in the HTTP `RelayWireRecord` (not inside `blob_b64`). The relay indexes on these fields; they are **not** included in E014 sign bytes.

| Field | Type | Required on send | Notes |
|-------|------|------------------|-------|
| `recipient_contact_id` | string | yes | Inbox owner — relay delivery target |
| `stream_id` | string | yes | Opaque conversation scope; v1 canonical: `v1:{channel}:{session_epoch}:{id_lo}:{id_hi}` with sorted relay ids |
| `index_key` | integer (u64) | yes | Monotonic per-stream ordering for gap fill; **v1:** equals `sender_seq` |

**Client builders:** `BuildCanonicalRelayStreamKey` (`base/messaging/RelayStreamKey.*`) maps to wire `stream_id`.

### `Route`

| `kind` | Fields | Maturity |
|--------|--------|----------|
| `direct` | `channel`: `e2e` \| `e2e_public` | **`[v1]`** private; **`[post-v1]`** public |
| `group` | `group_id`: string | **`[v1]`** (Bucket C) |

### `body.e2e` (direct tiers — D090, E024)

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `payload_b64` | string | yes | RFC 4648 base64 of `[version:1][nonce:24][ciphertext+tag]` (E009) |
| `key_init_b64` | string | no | **`e2e_public` only** — hybrid KEM encapsulation for recipient to derive `master_psk` when local PSK missing (E024). Relay may store/forward; must not learn PSK. **Not signed** (outside `body_hash` — hash covers `payload_b64` blob only). Omit on **`e2e` (private)** and when recipient already has PSK. |

### `body.e2e` (group tier — D095, E022)

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `member_payloads` | object map | yes | Keys = recipient relay ids; values = RFC 4648 base64 ciphertext blobs (pairwise `e2e_public` per member). Each value uses the same AEAD layout as direct `payload_b64`. |

**Rules:** `route.kind=group` requires `member_payloads`; omit `payload_b64` and `route.channel`. One envelope is fan-out per member on send (client may emit N relay records). See [group chat DESIGN](../../projects/group-chat/DESIGN.md).

---

## `ChatPayload` (v1 — binary, D087/D088)

Unified body for disk and E2E AEAD plaintext (D026, E010). Stored canonically in `messages.chat_payload` BLOB (D069/D078).

**Encoder:** **`ChatPayloadCodec::Encode`** / **`Decode`** in `base/messaging` using **`WireLenUtf8`** and **`OutputArchive`**.

### Binary layout (`payload_version = 1`)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `payload_version` = **`1`** |
| 1 | 1 | `content_type` enum — see table below |
| | var | `text` — **LenUtf8** |
| | var | **Type tail** — inline fields below (not a nested length blob) |

| `content_type` | Value | Maturity |
|----------------|-------|----------|
| `text` | `0` | **`[v1]`** |
| `system` | `1` | **`[v1]`** |
| `annotation` | `2` | **`[post-v1]`** |
| `contact_card` | `3` | **`[post-v1]`** |
| `crypto_tx` | `4` | **`[post-v1]`** |

**Rules:** reject unknown `content_type` on relay ingest (D050). Max serialized size: **`kMaxE2ePlaintextBytes`** (128 KiB) after decrypt (D029). Bump **`payload_version`** for breaking layout changes.

### Type tails (inline, v1)

**`content_type = text`:**

| Field | Encoding | Canonical rule |
|-------|----------|----------------|
| `sub_version` | `u8` = **`1`** | **Omit** when `format = plain` (0) — default |
| `format` | `u8` | **`0`** = plain |

**`content_type = system`:**

| Field | Encoding |
|-------|----------|
| `sub_version` | `u8` = **`1`** |
| `control_type` | **LenUtf8** |
| `detail` | **LenUtf8** (empty = zero length) |

**Group membership `control_type` values (Bucket C):** `group_invite`, `group_invite_accept`, `group_invite_decline`, `member_joined`, `member_left`, `member_removed`, `owner_transferred`, `group_renamed`, `group_forked`. Detail JSON schemas: [group chat DESIGN § Membership events](../../projects/group-chat/DESIGN.md#membership-event-wire-schema).

**`[post-v1]`** types: documented sub-layouts in [DESIGN § ChatPayload](../../projects/chat-storage-and-memory/DESIGN.md#chatpayload-unified-message-body--d026).

### Frozen vectors (BLAKE2b-256 of `0x01` ‖ bytes)

Regenerate: [`chatpayload_codec.py`](../../projects/e2e-message-crypto/tools/chatpayload_codec.py), [`gen_sign_vectors.py`](../../projects/e2e-message-crypto/tools/gen_sign_vectors.py).

#### Vector A — ASCII text, plain default (E014 / AEAD fixtures)

| Field | Value |
|-------|-------|
| Logical | `content_type=text`, `text="Hello"` |
| **`bytes` (hex)** | `0100000000000000000548656c6c6f` |
| **`body_hash` (E2E blob, hex)** | `b09daad4a14b17961c834c3b027c3d03ef49a0b1f3bffaa7c8c22da097a8042e` *(example — hash of `0x02` ‖ E2E blob)* |

#### Vector B — non-ASCII text, plain default

| Field | Value |
|-------|-------|
| Logical | `content_type=text`, `text="Café ☕"` |
| **`bytes` (hex)** | `01000000000000000009436166c3a920e29895` |

#### Vector C — system with LenUtf8 tail

| Field | Value |
|-------|-------|
| Logical | `content_type=system`, `text="Peer joined"`, `control_type="member_joined"`, `detail="alice"` |
| **`bytes` (hex)** | `0101000000000000000b50656572206a6f696e656401000000000000000d6d656d6265725f6a6f696e65640000000000000005616c696365` |
| **`body_hash` (hex)** | `b5bd1016b9ae31b1269638fe3fb86f44e1f74dfedc03bf3041812f885757453f` |

---

## `ChatHistoryRequest` (shared — libp2p D060; relay maps via `stream_key`)

Single request shape for **`FetchChatTargetMessages`** (D058) in pp-browser. **libp2p (D060):** UTF-8 JSON body on stream using the chat-shaped fields below. **HTTP relay (D027):** generic `GET /api/relay/v1/streams/messages` — the client maps this struct to relay query params (`requester_contact_id`, `sender_contact_id`, `stream_key`, `min_order_key`, `max_order_key`, `limit`, `order`). See [Stream history (HTTP relay)](#stream-history-http-relay) below.

Example libp2p / client JSON:

```json
{
  "requester_identity_kind": "relay_user",
  "requester_identity_value": "relay:local",
  "peer_identity_kind": "relay_user",
  "peer_identity_value": "relay:peer",
  "channel": "e2e",
  "session_epoch": 1,
  "min_sender_seq": 10,
  "max_sender_seq": 42,
  "limit": 50,
  "order": "asc"
}
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `requester_identity_kind` | string | yes | Authenticated caller identity kind |
| `requester_identity_value` | string | yes | Authenticated caller identity value |
| `peer_identity_kind` | string | yes | Other party identity kind |
| `peer_identity_value` | string | yes | Other party (`ChatTargetKey.peer_identity_value`) |
| `channel` | string | yes | `e2e` \| `e2e_public` |
| `session_epoch` | integer | yes | Required for both direct tiers |
| `min_sender_seq` | integer | no | Inclusive lower bound — mapped to relay `min_index_key` in v1 |
| `max_sender_seq` | integer | no | Inclusive upper bound — mapped to relay `max_index_key` in v1 |
| `limit` | integer | no | Default **50**, max **100** (D029) |
| `order` | string | no | `asc` (default) \| `desc` |

### Stream history (HTTP relay)

Relay is **chat-agnostic**. History is scoped by opaque `stream_id` + `index_key` (not `channel` / `session_epoch` on the wire).

```
GET /api/relay/v1/streams/messages?requester_contact_id=relay%3Abob&sender_contact_id=relay%3Aalice&stream_id=v1%3Ae2e%3A1%3Arelay%3Aalice%3Arelay%3Abob&min_index_key=10&max_index_key=42&limit=50&order=asc
```

| Query param | Type | Required | Notes |
|-------------|------|----------|-------|
| `requester_contact_id` | string | yes | Inbox owner (must differ from `sender_contact_id`) |
| `sender_contact_id` | string | yes | Stream sender whose messages are fetched |
| `stream_id` | string | yes | Opaque stream id (v1 canonical form above) |
| `min_index_key` | integer | no | Inclusive lower bound |
| `max_index_key` | integer | no | Inclusive upper bound |
| `limit` | integer | no | Default **50**, max **100** |
| `order` | string | no | `asc` (default) \| `desc` |

**MCP:** `relay_fetch_stream_history` with the same field names as the query params.

---

## `ChatHistoryResponse`

Client-facing response (libp2p and mapped relay). **HTTP relay** returns a stream-shaped JSON body; pp-browser merges chat fields from the request when absent.

Stream-shaped relay response:

```json
{
  "stream_id": "v1:e2e:1:relay:alice:relay:bob",
  "sender_contact_id": "relay:peer",
  "messages": [],
  "has_more": false,
  "cursor": {
    "next_min_index_key": null,
    "next_max_index_key": null
  }
}
```

Chat-shaped response (libp2p / documentation):

  "peer_identity_kind": "relay_user",
  "peer_identity_value": "relay:peer",
  "channel": "e2e",
  "session_epoch": 1,
  "messages": [],
  "has_more": false,
  "cursor": {
    "next_min_sender_seq": null,
    "next_max_sender_seq": null
  }
}
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `peer_identity_kind` | string | yes | Stream owner identity kind |
| `peer_identity_value` | string | yes | Stream owner identity value |
| `channel` | string | yes | |
| `session_epoch` | integer | yes | Both direct tiers |
| `messages` | `RelayInboundRecord[]` | yes | Opaque blobs; client unwraps to `RelayEnvelope` |
| `has_more` | boolean | yes | |
| `cursor` | object | yes | Pagination hints for caller |

**Implementation:** one C++ struct pair (`ChatHistoryRequest`, `ChatHistoryResponse`) shared by `IRelayClient` and libp2p host glue — do not fork field names per transport (D072).

---

## Versioning matrix

| Artifact | Version field | Bump when |
|----------|---------------|-----------|
| SQLite `thread.db` / `profile.db` | `PRAGMA user_version` | Column/table layout change — **migrate** (D069) |
| `RelayEnvelope` | `envelope_version` | Outer wire shape or signing canonical set changes |
| `ChatPayload` | `payload_version` | Body binary layout or required tail fields change (D087) |
| Binary wire profile | (per-format version byte) | LenUtf8 rules unchanged; bump format version on layout break |
| E2E AAD | `aad_version` | [e2e-message-crypto DESIGN](../../projects/e2e-message-crypto/DESIGN.md) |
| libp2p history | protocol id `/pp-browser/chat-history/1.0.0` | Request/response breaking change → new protocol id |
| App release | `PP_BROWSER_RELEASE_VERSION` | Packaging + About + User-Agent; compared to `min_client_version` |
| Client-compat | `schema_version` on `GET /v1/client-compat` | Product floor / soft update nudge ([SERVICE_ENDPOINTS](SERVICE_ENDPOINTS.md#client-compatibility-discovery)) |
| Protocol gen | `kProtocolGen` | App constant; future in-band messaging hello — not directory profile |

**Dev-only wipe (D016):** legacy JSON threads + pre-v1 wire — not the same as production `user_version` migration. See [COMPATIBILITY.md](COMPATIBILITY.md).
