# Wire schemas — normative JSON shapes

**Status:** Planning — v2a-p2p+  
**Authority:** Field behavior and validation rules live in [DESIGN.md](DESIGN.md); this file is the **canonical JSON shape reference** for implementers, relay API, and libp2p history (D072). C++ codecs in `base/messaging` must match these types.

**Related:** [e2e-message-crypto/DESIGN.md](../e2e-message-crypto/DESIGN.md) (AAD layout, ciphertext), [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md).

**Identity (D079, D082):** Wire **`sender_contact_id`** carries the sender's **communicating identity value** (e.g. `relay:abc123`) — not local `Contact.id`. **`Contact.id`** is address-book only. v1 relay: **`peer_identity_kind` = `relay_user`**, **`peer_identity_value` = `relay:` + opaque id** (relay-assigned, URL-safe; see [DECISIONS D082](DECISIONS.md#d082--relay-user-communicating-identity-string-format)).

---

## Unknown-field policy (D073)

| Layer | Rule |
|-------|------|
| **`RelayEnvelope`** | **Ignore** unknown top-level keys on ingest after required fields parse. Required keys must be present. Do not fail on forward-compatible extensions. |
| **`ChatPayload`** | **Ignore** unknown keys inside `payload` for known `content_type`. **Reject** unknown `content_type` on **relay ingest** (D030/D050). Local dev rows may use future types. |
| **`ChatHistoryRequest` / `ChatHistoryResponse`** | **Reject** unknown top-level keys (server/client negotiated API). |
| **Signature input** | Only documented canonical fields participate in signed bytes — unknown envelope keys are **not** signed unless a future `envelope_version` spec says otherwise. **Normative byte layout:** [e2e-message-crypto DESIGN § Ed25519 signing](../e2e-message-crypto/DESIGN.md#ed25519-canonical-signing-bytes) (E014). |

---

## `RelayEnvelope` (v1 — `envelope_version: 1`)

Signed outer wrapper. **No `thread_id`.** See [DESIGN § Relay envelope](DESIGN.md#relay--direct-envelope-d056-d063).

```json
{
  "envelope_version": 1,
  "message_id": "550e8400-e29b-41d4-a716-446655440000",
  "sender_relay_id": "relay:alice123",
  "sender_contact_id": "relay:alice123",
  "route": {
    "kind": "direct",
    "channel": "public_relay"
  },
  "body": {
    "content": { }
  },
  "timestamp": 1719662400123,
  "signature": "base64-ed25519"
}
```

**E2E direct** adds `sender_seq`, `session_epoch`; `route.channel` = `e2e`; `body` uses `{ "e2e": { "payload_b64": "…" } }` instead of `body.content`.

### Ed25519 canonical signing (E014)

Implement via **`EnvelopeSigner`** in `base/messaging`. **Do not** sign JSON `dump()` of the envelope.

| Topic | v1 rule |
|-------|---------|
| Sign bytes | Fixed binary: domain prefix + `sign_version` + fields — full layout in [e2e DESIGN § Ed25519 signing](../e2e-message-crypto/DESIGN.md#ed25519-canonical-signing-bytes) |
| Signed fields | `envelope_version`, `message_id`, `sender_contact_id`, `route` (as enums), `timestamp`, `body_hash`, `sender_seq`, `session_epoch` |
| Public channel seq/epoch | Wire **omits** `sender_seq`/`session_epoch`; signing uses **`0`** for both |
| `body_hash` | BLAKE2b-256(`body_kind` \|\| payload): `0x01` + canonical `ChatPayload` JSON for public; `0x02` + decoded E2E blob bytes |
| Not signed | `thread_id`, `sender_relay_id`, `signature`, unknown keys |
| `signature` encoding | Standard **base64** (RFC 4648, padded) only in v1 |

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `envelope_version` | integer | yes | **1** in v1. Signed (D072). Bump independently of `ChatPayload.schema_version`. |
| `message_id` | string (UUID) | yes | Dedup key (D034) |
| `sender_relay_id` | string | yes | Relay registration id; **v1:** same string as `sender_contact_id` (D082) |
| `sender_contact_id` | string | yes | Sender **communicating identity value** (D079) — e.g. `relay:abc123` (D082) |
| `route` | object | yes | See `Route` below |
| `body` | object | yes | `content` (public) or `e2e` (encrypted) |
| `sender_seq` | integer (u64) | E2E only | Omitted on public (D045) |
| `session_epoch` | integer (u32) | E2E only | |
| `timestamp` | integer (i64) | yes | Unix **milliseconds**; display metadata; not sort authority (D054). Included in sign bytes (E014). |
| `signature` | string | yes | Ed25519 over [canonical sign bytes](../e2e-message-crypto/DESIGN.md#ed25519-canonical-signing-bytes); **base64** (RFC 4648, padded) in v1 |
| `sender_instance_id` | string (UUID) | **`[future]`** | Multi-device extension (D074); omit in v1 |

### `Route`

| `kind` | Fields | Maturity |
|--------|--------|----------|
| `direct` | `channel`: `public_relay` \| `e2e` | **`[v1]`** |
| `group` | `group_id`: string | **`[post-v1]`** |

---

## `ChatPayload` (v1 — `schema_version: 1`)

Unified body for disk, relay plaintext, and E2E AEAD plaintext (D026, E010). Stored canonically in `messages.chat_payload_json` (D069).

```json
{
  "schema_version": 1,
  "content_type": "text",
  "text": "Hello",
  "payload": {}
}
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `schema_version` | integer | yes | **1** in v1 |
| `content_type` | string | yes | `text`, `system` **`[v1]`**; rich types **`[post-v1]`** |
| `text` | string | conditional | Required for `text`; snippet for other types |
| `payload` | object | yes | Type-specific; `{}` allowed for `text` |

**`[v1]` `content_type` payload shapes:** see [DESIGN § ChatPayload](DESIGN.md#chatpayload-unified-message-body--d026).

---

## `ChatHistoryRequest` (shared — relay GET + libp2p D060)

Single request shape for **`FetchChatTargetMessages`** (D058). HTTP: query params; libp2p: UTF-8 JSON body on stream.

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
| `channel` | string | yes | `public_relay` \| `e2e` |
| `session_epoch` | integer | E2E | Required when `channel=e2e` |
| `min_sender_seq` | integer | no | Inclusive lower bound |
| `max_sender_seq` | integer | no | Inclusive upper bound |
| `limit` | integer | no | Default **50**, max **100** (D029) |
| `order` | string | no | `asc` (default) \| `desc` |

---

## `ChatHistoryResponse`

```json
{
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
| `session_epoch` | integer | E2E | When `channel=e2e` |
| `messages` | `RelayEnvelope[]` | yes | No `thread_id` on elements |
| `has_more` | boolean | yes | |
| `cursor` | object | yes | Pagination hints for caller |

**Implementation:** one C++ struct pair (`ChatHistoryRequest`, `ChatHistoryResponse`) shared by `IRelayClient` and libp2p host glue — do not fork field names per transport (D072).

---

## Versioning matrix

| Artifact | Version field | Bump when |
|----------|---------------|-----------|
| SQLite `thread.db` / `profile.db` | `PRAGMA user_version` | Column/table layout change — **migrate** (D069) |
| `RelayEnvelope` | `envelope_version` | Outer wire shape or signing canonical set changes |
| `ChatPayload` | `schema_version` | Body schema or required payload keys change |
| E2E AAD | `aad_version` | [e2e-message-crypto](../e2e-message-crypto/DESIGN.md) |
| libp2p history | protocol id `/pp-browser/chat-history/1.0.0` | Request/response breaking change → new protocol id |

**Dev-only wipe (D016):** legacy JSON threads + pre-v1 wire — not the same as production `user_version` migration.
