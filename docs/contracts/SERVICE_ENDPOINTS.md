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
| Finish | `POST /v1/register/finish` | `{ challenge, public_key, kem_public_key_b64, signature, timestamp, nickname?, signature_alg? }` | `{ success, relay_user_id, message, expires_at }` |

Finish signs canonical bytes: domain `pp-browser:relay-register-v1\0`, `sign_version=2`, challenge (len-prefixed UTF-8), 32-byte raw Ed25519 public key, 1216-byte raw hybrid KEM public key, `signature_alg` u8 (`0=ed25519`), `timestamp` i64 BE.

## HTTP relay API auth (per-request sign bytes)

All relay API calls require `timestamp` + `signature` over `pp-browser:relay-api-v1\0` canonical bytes.

| Op | HTTP | Signed fields |
|----|------|----------------|
| send | `POST /v1/messages` | sender, recipient, stream_id, index_key |
| poll_inbox | `POST /v1/inbox/poll` | requester_contact_id, cursor |
| stream_history | `POST /v1/streams/messages/query` | requester, sender, stream_id, optional index range, limit, order |

`blob_b64` is **not** included in transport auth (E014 envelope signature covers message integrity inside the blob).

## Directory and profile

| HTTP | Purpose |
|------|---------|
| `GET /v1/search?q=` | Search relay users (`hits[]` with `signing_public_key_b64`, `kem_public_key_b64`, `relay_user_id`, `nickname`) |
| `GET /v1/users/:relay_user_id` | Public lookup (`signing_public_key_b64`, `kem_public_key_b64`, nickname, expires_at) |
| `POST /v1/profile/nickname` | Update nickname (`relay-profile-v1` sign bytes + signature) |

## Native agent tools

[`MessagingTools`](../../src/feature/ai/tools/MessagingTools.cpp) exposes `search_people`, `register_user`, and `update_profile_nickname` as native C++ tools calling `MessagingHub` → `Http*Client` directly (not via MCP).

## MCP client buckets

| Bucket | Config | Agent tools |
|--------|--------|-------------|
| Promoted | `promoted_mcp` (+ platform default) | Feed/AI tools (not relay infra) |
| Custom | `mcp_servers[]` | All tools; collisions prefixed as `{id}__{tool}` |

## libp2p (deferred)

Future work adds `Libp2p*Client` implementations behind the same interfaces and sign-byte auth over libp2p HTTP to www.
