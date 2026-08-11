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
  "directory": { "base_url": "https://www.brief.global/api/relay" },
  "registration": { "base_url": "https://www.brief.global/api/relay" }
}
```

`transport` is reserved for future libp2p support (`http` | `libp2p`). v1 uses HTTP when `base_url` is set.

## HTTP registration (challenge + sign bytes)

When `registration.base_url` is set (e.g. `https://host/api/relay`), `HttpRegistrationClient` uses a 2-step challenge flow:

| Step | HTTP | Request body | Response |
|------|------|--------------|----------|
| Start | `POST /v1/register/start` | `{ public_key, kem_public_key_b64, nickname?, signature_alg? }` | `{ challenge, signature_alg, expires_at }` |
| Finish | `POST /v1/register/finish` | `{ challenge, public_key, kem_public_key_b64, signature, timestamp, nickname?, signature_alg?, initiation_floor? }` | `{ success, relay_user_id, message, expires_at, llm_api_key, initiation_floor? }` |

Finish signs canonical bytes: domain `pp-browser:relay-register-v1\0`, `sign_version=2`, challenge (len-prefixed UTF-8), 32-byte raw Ed25519 public key (device/register proof today), **1184-byte raw ML-KEM-768** public key (`kem_public_key_b64`), `signature_alg` u8 (`0=ed25519`), `timestamp` i64 BE. (Pre-release: replaced 1216-byte X25519+Kyber-draft hybrid; Brief must accept 1184.)

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

Inbox is a **delivery queue**: poll advances a cursor; clients may `ack` through that cursor to delete consumed rows. `clear` deletes recipient rows older than a timestamp (recovery). Messenger also applies a 14-day TTL on `created_at`. Chat history remains local + stream/P2P sync — not the inbox.

Poll response extras (unix ms, relay clock):

- each inbound record may include `created_at` (when the relay stored the row)
- poll body includes `server_time` (relay clock at response build; present on empty polls too)

Call invite age uses `server_time - created_at` when both are present (caller create ≈ relay store).

## Directory and profile

| HTTP | Purpose |
|------|---------|
| `GET /v1/search?q=` | Search relay users (`hits[]` with `signing_public_key_b64`, `kem_public_key_b64`, `relay_user_id`, `nickname`, optional `peer_id` in `ids[]`, optional `multiaddrs`, optional `initiation_floor`) |
| `GET /v1/users/:relay_user_id` | Public lookup (`signing_public_key_b64`, `kem_public_key_b64`, nickname, expires_at, optional `peer_id`, `multiaddrs`, optional `initiation_floor`) |
| `POST /v1/profile/nickname` | Update nickname (`relay-profile-v1` sign bytes + signature) |
| `POST /v1/register/start` | Start registration (`public_key`, `kem_public_key_b64`, optional `nickname`, `peer_id`, `multiaddrs`) |
| `POST /v1/register/finish` | Finish/renew registration (same optional reachability fields; unsigned advisory) |

Peer protocol / app-version capability is **not** a directory concern. Peers discover mismatch via messaging / libp2p (soft-skip, protocol ids); the relay stays format-blind for that.

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

[`MessagingTools`](../../src/feature/chat/MessagingTools.cpp) exposes `search_people`, `register_user`, and `update_profile_nickname` as native C++ tools calling `MessagingHub` → `Http*Client` directly (not via MCP).

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
| Auth | `Authorization: Bearer <brf_llm_…>` issued on directory registration finish |
| Issue | Opaque key returned once in `register/finish` as `llm_api_key`; stored hashed on `RelayUser` |
| Rotate | `POST /api/llm/v1/keys/rotate` with current Bearer; returns new `llm_api_key` once |
| Limits | Global 300 req/min (`llm:global`); per-user 30 req/min (`llm:user:{id}`). Env: `BRF_WWW_LLM_GLOBAL_RPM`, `BRF_WWW_LLM_USER_RPM` |
| Errors | `401` invalid key, `403` registration expired (`code: not_registered`), `429` (`global_rate_limit` / `user_rate_limit`), `400` if `stream: true` |

Upstream is user-ai `POST /v1/chat/completions` (stateless; www→user-ai uses service token). pp-browser stores the plaintext key in profile `identity.enc` (`brief_llm_api_key`) and sends standard Bearer auth when preset is `brief`.

Wire format is OpenAI chat completions, including client tool loops (`assistant.tool_calls` + `role: tool`). www does not adapt messages; user-ai maps them to the configured provider (xAI or openai-compatible).

Lost key or expired registration: use **Renew registration** in Me → Profile (finish) to issue a new key. **Rotate Brief API key** remains available while registration is active.

## libp2p (deferred)

Future work adds `Libp2p*Client` implementations behind the same interfaces and sign-byte auth over libp2p HTTP to www.
