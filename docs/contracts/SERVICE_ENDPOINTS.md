# Service endpoints

**Tier:** contract

Relay, directory, and registration share a common resolution pattern. All client creation goes through [`CreateServiceClients`](../../src/base/net/ServiceClientFactory.cpp) so future libp2p transports can plug in without changing messaging or agent code.

## Resolution order

For each of `relay`, `directory`, and `registration`:

| Priority | Condition | Implementation |
|----------|-----------|----------------|
| 1 | `base_url` non-empty (platform default or `config.json`) | `Http*Client` |
| 2 | otherwise | Client not created (`CreateServiceClients` leaves unset); production defaults always fill Brief URLs |

Platform defaults (`PlatformDefaults`) set all three to `https://www.brief.global/api/relay`. Empty values in settings/config coalesce back to those defaults. `Mock*Client` implementations remain for **unit tests only** (construct directly); they are not selected by the factory.

## Config shape

```json
{
  "relay": { "base_url": "https://www.brief.global/api/relay", "transport": "http" },
  "directory": {
    "base_url": "https://www.brief.global/api/relay",
    "transport": "http",
    "providers": [
      { "base_url": "https://www.brief.global/api/relay", "transport": "http" }
    ]
  },
  "registration": { "base_url": "https://www.brief.global/api/relay" }
}
```

`directory.providers[]` (N029 nd3) is an **ordered failover list**. When present and non-empty, `CreateServiceClients` builds HTTP clients for each `transport: http` entry and wraps them in `FailoverDirectoryClient`. When `providers` is omitted/empty, behavior matches legacy single `directory.base_url`. Non-`http` transports (e.g. future `amp`) are skipped with a warning until Phase B.

`transport` on relay/registration remains reserved (`http` now). Directory providers use the same vocabulary.

## HTTP registration (challenge + sign bytes)

When `registration.base_url` is set (e.g. `https://host/api/relay`), `HttpRegistrationClient` uses a 2-step challenge flow:

| Step | HTTP | Request body | Response |
|------|------|--------------|----------|
| Start | `POST /v1/register/start` | `{ public_key, kem_public_key_b64, nickname?, signature_alg? }` | `{ challenge, signature_alg, expires_at }` |
| Finish | `POST /v1/register/finish` | `{ challenge, public_key, kem_public_key_b64, signature, timestamp, nickname?, signature_alg?, initiation_floor? }` | `{ success, relay_user_id, message, expires_at, llm_api_key, initiation_floor? }` |

Finish signs canonical bytes: domain `pp-browser:relay-register-v1\0`, `sign_version=2`, challenge (len-prefixed UTF-8), **1952-byte raw ML-DSA-65** account public key, **1184-byte raw ML-KEM-768** **account** public key (`kem_public_key_b64` — person encapsulate-to, **M015**; not per-device), `signature_alg` u8 (`1=ml-dsa-65`), `timestamp` i64 BE. Pre-release hard cut: Ed25519 register/API auth removed; wipe legacy relay_users.

Brief derives and stores `account_id = account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 pk))>` and binds **at most one** `relay_user_id` per Account ID (M006). Directory / user lookup include `account_id`, `signature_alg=ml-dsa-65`, and the **account** `kem_public_key_b64` (same key on every linked device).

## HTTP relay API auth (per-request sign bytes)

All relay API calls require `timestamp` + `signature` over `pp-browser:relay-api-v1\0` canonical bytes.

| Op | HTTP | Signed fields |
|----|------|----------------|
| send | `POST /v1/messages` | sender, recipient, stream_id, index_key |
| poll_inbox | `POST /v1/inbox/poll` | requester_contact_id, cursor |
| ack_inbox | `POST /v1/inbox/ack` | requester_contact_id, cursor |
| clear_inbox | `POST /v1/inbox/clear` | requester_contact_id, before_created_at (ISO-8601) |
| stream_history | `POST /v1/streams/messages/query` | requester, sender, stream_id, optional index range, limit, order |
| device_register | `POST /v1/devices/register` | relay_user_id, platform, device_id, push_token |
| device_unregister | `POST /v1/devices/unregister` | relay_user_id, platform, device_id, push_token |

`blob_b64` is **not** included in transport auth (E014 envelope signature covers message integrity inside the blob).

Inbox is a **delivery queue**: poll advances a **per-device** local cursor; `ack` is **soft** (M013 — validate only; no shared delete). `clear` deletes recipient rows older than a timestamp (recovery; account-wide). Messenger also applies a **90-day TTL** on `created_at` (startup rewrites the TTL index only if needed) and a **soft per-recipient FIFO cap** (trim toward **1000** when a mailbox exceeds **~1200**). Chat history remains local + stream/P2P sync — not the inbox.

Poll response extras (unix ms, relay clock):

- each inbound record may include `created_at` (when the relay stored the row)
- poll body includes `server_time` (relay clock at response build; present on empty polls too)

Call invite age uses `server_time - created_at` when both are present (caller create ≈ relay store).

## Directory and profile

| HTTP | Purpose |
|------|---------|
| `GET /v1/search?q=` | Search **people** only (`entity_kind` person / missing). Matches nickname, `relay:`, Account ID. Hits include `endpoints[]`, keys, optional `entity_kind`. **Excludes** `mesh_node` ([N027](../../projects/p2p-mesh/DECISIONS.md#n027--mesh-directory-entity_kind-pluggable-providers-bootstrapdirectory)) |
| `GET /v1/mesh/nodes` | List non-expired **`mesh_node`** infra listings: `account_id`, `relay_user_id`, `nickname`, `endpoints[]`, `capabilities` (`circuit_relay` / `media_relay`), `expires_at`, keys |
| `GET /v1/users/by-account/:account_id` | Lookup by Account ID (any kind): keys, `relay_user_id`, `signature_alg`, nickname, expires_at, **`endpoints[]`**, optional `entity_kind` / `capabilities` |
| `GET /v1/users/:relay_user_id` | **Route** lookup by `relay:` id (same fields; **`account_id` required** when bound) |
| `POST /v1/profile/nickname` | Update nickname (`relay-profile-v1` sign bytes + signature) |
| `POST /v1/register/start` | Start registration (`public_key`, `kem_public_key_b64`, optional `nickname`, `peer_id`, `multiaddrs`, optional `entity_kind` / `capabilities`) |
| `POST /v1/register/finish` | Finish/renew (`entity_kind` default `person`; `mesh_node` for pp-node). Upserts `endpoints[]` by Peer ID; persists capabilities for mesh nodes; echoes **`account_id`** |

**Directory providers:** Brief HTTP is the default (`directory.base_url`). Clients should treat directory as pluggable (N027); bootstrap peers remain L0 cold-start / emergency dial, not the long-term sole mesh-service discovery path.

Peer protocol / app-version capability is **not** a directory concern. Peers discover mismatch via messaging / libp2p (soft-skip, protocol ids); the relay stays format-blind for that.

## HTTP relay blobs (icons + chat attachments)

Base path under the same `{registration.base_url}` / `{relay.base_url}` (e.g. `https://www.brief.global/api/relay/v1`). Client: `HttpBlobClient` / `IBlobClient` ([relay-blob-upload](../../projects/relay-blob-upload/)). Server never inspects ciphertext for chat files (`application/octet-stream`).

Auth: separate sign domains (not `relay-api-v1`). Canonical bytes via `RelayBlobSignPayload` — domain + u8 version=`1`, then length-prefixed UTF-8 / BE integers matching www `SignatureVerifier`.

| Domain | Signed fields |
|--------|-----------------|
| `pp-browser:relay-blob-presign-v1` | `relay_user_id`, `content_type`, `byte_length` (u64), `purpose`, `timestamp` (i64) |
| `pp-browser:relay-blob-retain-v1` | `relay_user_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-blob-delete-v1` | `relay_user_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-blob-list-v1` | `relay_user_id`, `status_filter` (empty if none), `timestamp` |
| `pp-browser:relay-profile-icon-v1` | `relay_user_id`, `url`, `blob_id`, `kind`, `timestamp` (empty strings when unused) |

| HTTP | Purpose |
|------|---------|
| `POST /v1/blobs/presign` | Mint `blob_id`, `upload_url`, `public_url`, `tier`, `pending_expires_at` |
| `PUT {upload_url}` | Client → S3 (binary body; `Content-Type` / length must match presign) |
| `POST /v1/blobs/retain` | Mark retained after successful PUT |
| `POST /v1/blobs/delete` | Free quota / remove object (local chat history untouched) |
| `POST /v1/blobs/list` | Inventory + usage (quota UX) |
| `POST /v1/profile/icon` | Attach hosted blob / external https, or clear |

Quotas (server defaults): small ≤ 4 MiB (≤ 100 objects); large ≤ 256 MiB with 2 GiB included; icon ≤ 512 KiB; pending TTL ~48h; retained GC ~90d (skip current profile icon). Presign **429** → client confirm → delete oldest remote ([R009](../../projects/relay-blob-upload/DECISIONS.md#r009--sender-always-retains-quota-pop-is-relay-only-with-confirm)).

Chat attachment **file bytes** prefer peer-direct `/pp-browser/chat-blob/1.0.0` then CDN ([R019](../../projects/relay-blob-upload/DECISIONS.md#r019--peer-first-blob-transfer-cdn-secondary)); the small attachment envelope stays on the normal message path.

## Client compatibility discovery

Unauthenticated public GET (directory-style; no identity unlock required):

`GET {relay.base_url}/v1/client-compat`

```json
{
  "schema_version": 1,
  "min_client_version": "0.3.0",
  "latest_client_version": "0.4.2",
  "min_protocol_gen": 1,
  "upgrade_url": "https://github.com/people-post/pp-browser/releases",
  "message": ""
}
```

| Field | Required | Notes |
|-------|----------|--------|
| `schema_version` | yes | **1**; reject unsupported **newer** |
| `min_client_version` | no | Semver floor; client below → blocking update UX |
| `latest_client_version` | no | Semver latest; client below (but ≥ min) → soft banner |
| `min_protocol_gen` | no | Global protocol floor (default 1) |
| `upgrade_url` | no | Empty → GitHub Releases fallback |
| `message` | no | Empty → client locale strings |

Unknown keys are ignored. Network failure: fail open (no gate); use last good profile cache (`client_compat.json`, TTL 6h) when present.

## HTTP device push (opaque wake)

Signed with `pp-browser:relay-api-v1` ops `DeviceRegister=3` / `DeviceUnregister=4` over `(relay_user_id, platform, device_id, push_token)`.

| HTTP | Purpose |
|------|---------|
| `POST /v1/devices/register` | Bind FCM token `{ platform, device_id, push_token, relay_user_id, timestamp, signature }` |
| `POST /v1/devices/unregister` | Remove device binding (alerts off / logout) |

On `POST /v1/messages` accept, a conforming relay **best-effort** sends an FCM **data** message to registered device tokens for the recipient:

| Condition | FCM data |
|-----------|----------|
| Ordinary message / default | `{ "type": "inbox_wake" }` |
| Call-invite class system control (`control_type=call_invite`) or dedicated call-invite accept path | `{ "type": "call_wake" }` |

Payloads remain **opaque** — no `call_id`, names, thread ids, or media. Failures must not fail message store. How the provider authenticates to FCM is outside this contract. See [projects/push-notifications](../../projects/push-notifications/) and [p2p-av-calls V006](../../projects/p2p-av-calls/DECISIONS.md).

## Native agent tools

[`MessagingTools`](../../src/gui/chat/MessagingTools.cpp) exposes `search_people`, `register_user`, and `update_profile_nickname` as native C++ tools calling `ConversationsHub` → `Http*Client` directly (not via MCP).

## MCP client buckets

| Bucket | Config | Agent tools |
|--------|--------|-------------|
| Promoted | `promoted_mcp` (+ platform default) | Feed/AI tools (not relay infra) |
| Custom | `mcp_servers[]` | All tools; collisions prefixed as `{id}__{tool}` |

## Brief LLM gateway

Default assistant preset uses OpenAI-compatible completions at:

`POST https://www.brief.global/api/llm/v1/chat/completions`

| Requirement | Detail |
|-------------|--------|
| Auth | `Authorization: Bearer <brf_llm_…>` (registration) **or** `Bearer <brf_guest_…>` (free tier) |
| Guest mint | `POST /api/llm/v1/guest/start` — no Bearer; returns `{ llm_api_key, expires_at, guest: true }` once |
| Issue (registered) | Opaque key returned once in `register/finish` as `llm_api_key`; stored hashed on `RelayUser` |
| Rotate | `POST /api/llm/v1/keys/rotate` with registered Bearer only; guest → `403 guest_rotate_unsupported` (remint via `/guest/start`) |
| Limits | Global 300 req/min (`llm:global`); registered 30/min (`llm:user:{id}`); guest 10/min + 50/day (`llm:guest:…`). Mint 5/IP/hour. Env: `BRF_WWW_LLM_*` |
| Errors | `401` invalid key, `403` registration/guest expired, `429` (`global_rate_limit` / `user_rate_limit` / `guest_rate_limit` / `guest_daily_limit` / `guest_mint_rate_limit`), `400` if `stream: true` |

Upstream is AI harness `POST /v1/chat/completions` (stateless; www→harness uses `X-Harness-Token`). pp-browser stores:

- Registered key in `identity.enc` as `brief_llm_api_key`
- Guest key as `brief_llm_guest_api_key` (auto-minted when preset is Brief and registered key is empty)

Overlay prefers registered over guest. Soft banner for free tier; register for higher limits.

Wire format is OpenAI chat completions, including client tool loops (`assistant.tool_calls` + `role: tool`). www does not adapt messages; the harness maps them to the configured provider.

Lost registered key or expired registration: use **Renew registration** in Me → Profile (finish) to issue a new key. **Rotate Brief API key** remains available while registration is active. Guests remint via `/guest/start` (app caches until expiry / register).

## libp2p (deferred)

Future work adds `Libp2p*Client` implementations behind the same interfaces and sign-byte auth over libp2p HTTP to www.
