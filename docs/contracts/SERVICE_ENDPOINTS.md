# Service endpoints

**Tier:** contract

Relay, directory, and registration share a common resolution pattern. Code calls the ports (`IRelayClient` — messaging's, `lib/delivery/RelayClients.h`; `IDirectoryClient`; `IRegistrationClient`), never an implementation, so other transports can plug in without changing messaging or agent code. The app builds the HTTP clients (`ConversationsHub`); [`CreateOrgBackendClients`](../../capabilities/net/src/lib/net/OrgBackendClientFactory.cpp) builds net's own (directory, registration, blobs, client compat) the same way.

## Resolution order

For each of `relay`, `directory`, and `registration`:

| Priority | Condition | Implementation |
|----------|-----------|----------------|
| 1 | `base_url` non-empty (platform default or `config.json`) | `Http*Client` |
| 2 | otherwise | Client not created (left unset); production defaults always fill Brief URLs |

With `backend.env` `prod` or `qa` the three are derived from that backend's compiled origin and the saved values are ignored; `custom` uses the saved ones ([CONFIGURATION.md](../ops/CONFIGURATION.md)). Push tokens are registered with the running backend's `/v1/devices/register` under the active profile's identity, so a backend switch (applied on restart) registers with the new one; when a switch is saved the device's bindings are removed from the running backend at once (`/v1/devices/unregister`, signed by the running profile, whose key is not usable after the restart) and registration stays off until the switch is undone, which registers again. An unreachable old backend keeps its binding: the removal is best effort and is retried when a switch is saved again.

Platform defaults (`PlatformDefaults`) set all three to `<origin>/api/relay` (e.g. `https://backend.example.test/api/relay`); the origin is empty, and the services disabled, unless supplied. Empty values in settings/config coalesce back to those defaults. `Mock*Client` implementations remain for **unit tests only** (construct directly); they are not selected by the factory.

## Config shape

```json
{
  "relay": { "base_url": "https://backend.example.test/api/relay", "transport": "http" },
  "directory": {
    "base_url": "https://backend.example.test/api/relay",
    "transport": "http",
    "providers": [
      { "base_url": "https://backend.example.test/api/relay", "transport": "http" }
    ]
  },
  "registration": { "base_url": "https://backend.example.test/api/relay" }
}
```

`directory.providers[]` (N029 nd3) is an **ordered failover list**. When present and non-empty, `CreateOrgBackendClients` builds HTTP clients for each `transport: http` entry and wraps them in `FailoverDirectoryClient`. When `providers` is omitted/empty, behavior matches legacy single `directory.base_url`. Non-`http` transports (e.g. future `amp`) are skipped with a warning until Phase B.

`transport` on relay/registration remains reserved (`http` now). Directory providers use the same vocabulary.

## HTTP registration (challenge + sign bytes)

When `registration.base_url` is set (e.g. `https://host/api/relay`), `HttpRegistrationClient` uses a 2-step challenge flow:

| Step | HTTP | Request body | Response |
|------|------|--------------|----------|
| Start | `POST /v1/register/start` | `{ public_key, kem_public_key_b64, nickname?, signature_alg? }` | `{ challenge, signature_alg, expires_at }` |
| Finish | `POST /v1/register/finish` | `{ challenge, public_key, kem_public_key_b64, kem_binding_signature_b64?, signature, timestamp, nickname?, signature_alg?, initiation_floor? }` | `{ success, relay_user_id, message, expires_at, llm_api_key, initiation_floor?, kem_bound }` |

Finish signs canonical bytes: domain `pp-browser:relay-register-v1\0`, `sign_version=2`, challenge (len-prefixed UTF-8), **1952-byte raw ML-DSA-65** account public key, **1184-byte raw ML-KEM-768** **account** public key (`kem_public_key_b64` — person encapsulate-to, **M015**; not per-device), `signature_alg` u8 (`1=ml-dsa-65`), `timestamp` i64 BE. Pre-release hard cut: Ed25519 register/API auth removed; wipe legacy relay_users.

Brief derives and stores `account_id = account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 pk))>` and binds **at most one** `relay_user_id` per Account ID (M006). Directory / user lookup include `account_id`, `signature_alg=ml-dsa-65`, and the **account** `kem_public_key_b64` (same key on every linked device) with `kem_binding_signature_b64`: the account's signature binding that KEM key to it ([MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md), `pp-browser:kem-binding-v1`). Finish verifies a binding it is sent and stores it with the KEM key, replacing both (none sent, none kept); `kem_bound` says the entry now carries one. Clients take a directory KEM key only with a valid binding, and a registration from before bindings re-registers once to publish one.

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
| `POST /v1/register/revoke` | Remove this account's registration and its push devices (profile reset / delete account, #328). Body `{ relay_user_id, timestamp, signature }`; signature over `pp-browser:relay-revoke-v1\0`, `u8 1`, `relay_user_id` (len-prefixed UTF-8), `i64 timestamp`, by the account key. Best effort on the client: a failure does not block the local reset |

**Directory providers:** Brief HTTP is the default (`directory.base_url`). Clients should treat directory as pluggable (N027); bootstrap peers remain L0 cold-start / emergency dial, not the long-term sole mesh-service discovery path.

Peer protocol / app-version capability is **not** a directory concern. Peers discover mismatch via messaging / libp2p (soft-skip, protocol ids); the relay stays format-blind for that.

## HTTP relay blobs (profile photos)

Base path under the same `{registration.base_url}` / `{relay.base_url}` (e.g. `https://backend.example.test/api/relay/v1`). Client: `HttpBlobClient` / `IBlobClient` ([relay-blob-upload](../../projects/relay-blob-upload/)). **Only profile photos use these routes now** (`purpose=icon`, then `/v1/profile/icon`); chat attachments go through [HTTP relay files](#http-relay-files-chat-attachments) and never call `/v1/blobs/presign`. The `purpose=file` rows below describe what the relay still accepts; the client no longer sends them.

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

`public_url` is issued by the relay (a CDN URL) and is the only hosted-icon URL: clients store it as is and never rewrite it. `POST /v1/profile/icon` with a hosted `blob_id` may omit `url`; a `url` that differs from the issued `public_url` is refused with 400 `Hosted blob URL does not match`.

Quotas (server defaults; the client now presigns profile photos only, so for it only the icon limit applies, and the small / large tiers describe what the relay still accepts for `purpose=file`): small ≤ 4 MiB (≤ 100 objects); large ≤ 256 MiB with 2 GiB included; icon ≤ 512 KiB; pending TTL ~48h (and ≤ 20 pending uploads); retained GC ~90d (skip current profile icon). The tier follows `byte_length` (the ciphertext), so a client that stays at 4 MB per file only ever fills the small tier. The large tier has **no overage billing today**: a presign that would pass `large_included_bytes` is refused with **429** `Large file storage quota exceeded` (`usage.overage_bytes` is always 0). A blob is released only by `POST /v1/blobs/delete` (also "delete for everyone" in the client, `DeleteUploadedCopy`) or by the 90-day GC; "delete for me", clearing history and deleting a thread do not touch the server copy.

Failure statuses and the client's typed error (`CheckBlobResponse` → `ClassifyBlobFailure`, `Err::Blob`; the log detail is `HTTP <status>: <error text>`). The server's text, not only the status, picks the code where one status has several causes:

| Status (server `error` text) | Produced by | `Err::Blob` | Client does |
|---|---|---|---|
| 503 `Blob upload is not configured` (an older gateway: 400, same text) | every `/v1/blobs/*` route, and `/v1/profile/icon` with a hosted blob, when the relay has no upload bucket (external https icons and clearing are unaffected) | `UploadNotConfigured` | failure line "server hasn't enabled uploads" |
| 403 `Blob not owned by caller`, `Relay user not registered`, `Relay registration expired`; also a 403 from the storage PUT (expired or mismatched signed URL) | retain / delete, request auth, S3 | `UploadRejected` | failure line, retry later |
| 404 `Blob not found` | retain / delete (e.g. the pending upload expired); presign for an unknown relay user | `Expired` | failure line; a CDN 404 on download is not an API answer (typed in the download step) |
| 400 `File exceeds size limit` (also 413 if a proxy sends it; the relay itself never answers 413) | presign above `largeMaxBytes` | `TooLarge` | not retried |
| 429 `Small file quota exceeded` / `Large file storage quota exceeded` (these two exact texts only) | presign | `QuotaExceeded` | `BlobQuotaRecoveryFlow`: confirm, delete the oldest non-icon relay blob of the tier named in the 429 text (small / large; the blob id shown in the confirm is the one deleted), upload again; the confirm says people who have not downloaded it yet will not get it |
| any other 429: `Rate limit exceeded` (120 per user per minute), `Too many pending uploads`, an empty or proxy / WAF body | every blobs route / presign | `RateLimited` | "sending too fast", no delete offered |
| other 5xx | gateway / relay failure | `Unavailable` | failure line, retry |
| anything else (400 `Unsupported content type`, 400 `Hosted blob URL does not match`, 401 signature errors, …) | | `Generic` | failure line |

Chat attachments do not use these routes (their failures are in the relay-files table below), so the 429 quota rows above no longer come from a chat send. A space warning still reads `usage` from `POST /v1/blobs/list` after a successful upload (`CheckBlobSpace`; that list does not count relay files, so it does not reflect attachments today) (at most once a day, and after every 429 quota answer): percent of `large_included_bytes` for the large tier, and of the client's `small_max_objects` (default 100) for the small tier, because the list answer carries no small-tier limit (ask the relay to add one). The client tells the person once at 80 % and once at 90 %.

**Transfer of the ciphertext (client).** The upload `PUT` to the `upload_url` from `files/start` / `files/part` is streamed from a file (`HttpClient::PutFile`, `IBlobClient::PutUploadFile`): no whole-body string, progress reported as bytes go out, no total time limit, but a 15 s connect timeout and a speed floor (under 2 KB/s over 30 s aborts). The download `GET` of the message's relay download link (302 to storage) goes to a temp file (`HttpClient::GetToFile`) with a hard cap of the declared plaintext size plus the 16-byte AEAD tag: a larger `Content-Length` is refused before any byte is read, a body that grows past the cap is aborted and the temp file deleted, and a non-2xx answer writes nothing. `kMaxHttpClientBodyBytes` (8 MiB) still bounds every buffered request.

Chat attachment **file bytes** prefer peer-direct `/pp/blob/1.0.0` then CDN ([R019](../../projects/relay-blob-upload/DECISIONS.md#r019--peer-first-blob-transfer-cdn-secondary)); the small attachment envelope stays on the normal message path.

## HTTP relay files (chat attachments)

The encrypted-file relay (app-static-api `docs/relay-files.md`, routes `app/src/r_relay.ts`), same base path. Client: `IBlobClient::FileStart` / `FilePart` / `FileComplete` / `FileDelete` / `FileList` (`HttpBlobClient`), `UploadRelayFile` / `UploadRelayFileBytes` (`lib/net/BlobClient.h`). The server stores opaque ciphertext in a private bucket.

Signed calls (`POST`, JSON body with `relay_user_id`, `timestamp` (epoch ms), `signature`): sign bytes are `domain 0x00 | u8(1) | fields | i64be timestamp`, strings `u64be(len) | utf8`, integers `u64be` (`RelayBlobSignPayload`; golden vectors from the server's `RelayFiles.test.ts` in `relay_file_sign_payload_test.cpp`).

| HTTP | Domain | Signed fields | Answer |
|------|--------|---------------|--------|
| `POST /v1/files/start` (`byte_length`) | `pp-browser:relay-file-start-v1` | `relay_user_id`, `byte_length` (u64) | `{ blob_id, mode, part_bytes, part_count, upload_url?, expires_in, pending_expires_at }` |
| `PUT {upload_url}` | — | — | `mode=single`: the ciphertext, exactly `byte_length` bytes (`Content-Length` is signed into the URL; valid 15 min) |
| `POST /v1/files/part` (`blob_id`, `part_number`) | `pp-browser:relay-file-part-v1` | `relay_user_id`, `blob_id`, `part_number` (u64, 1-based) | `{ upload_url, part_bytes, expires_in }`: a PUT URL for that part (valid 1 h); `part_bytes` is **this part's** size (the plan's `part_bytes` except the last, which is the remainder); 400 `INVALID_PART_NUMBER` outside 1..`part_count` |
| `PUT {part upload_url}` | — | — | `mode=multipart`: bytes `(n-1)·part_bytes` … of the ciphertext, exactly that part's size, no `Content-Range`; the relay assembles the parts at `complete` (no ETags kept) |
| `POST /v1/files/complete` (`blob_id`) | `pp-browser:relay-file-complete-v1` | `relay_user_id`, `blob_id` | `{ blob_id, token, share_path, expires_at }`, **once**: a repeat is 400 `BLOB_NOT_PENDING` |
| `POST /v1/files/delete` (`blob_id`) | `pp-browser:relay-file-delete-v1` | `relay_user_id`, `blob_id` | `{ success: true }`; frees the quota, the link stops working |
| `POST /v1/files/list` (`status?` `pending` / `complete`) | `pp-browser:relay-file-list-v1` | `relay_user_id`, `status` (`""` when omitted) | `{ files: [{ blob_id, status, byte_length, created_at, completed_at, expires_at, pending_expires_at, … }], usage: { bytes_used, pending_count, quota_bytes, max_pending } }`, newest first, at most 500, no tokens |
| `GET /v1/files/<blob_id>/download?t=<token>` | unsigned | — | **302** to a 60-second storage URL (`Cache-Control: no-store`); every failure (unknown, expired, used up, deleted) is the same **404** `{"error":"Not found"}` |

`share_path` is `/api/relay/v1/files/<blob_id>/download?t=<token>`; the message's `url` is the relay origin (the relay base URL without `/api/relay`) plus it. **The token is the download credential**: logs, errors and metrics carry the blob id, status codes and `RelayFileUrl::Redacted` (no query) only, and the redirect target is never logged.

Limits (server, fixed): retention 7 days from `complete` (downloads do not extend it), **10 downloads per file** (each 302 counts, finished or not), 1 GiB per account (pending + complete), 20 pending uploads, pending TTL 48 h, single PUT up to 16 MiB, multipart above it with 16 MiB parts, 512 MiB per file. The client sends at most 50 MB per file (`kLargeAttachmentMaxBytes`), so it gets `mode=single` or a multipart plan of up to 3 parts; ciphertext held only in memory (`UploadRelayFileBytes`, at most 4 MiB) treats `multipart` as an error.

Client flow (`UploadRelayFile`): `start(byte_length = ciphertext length)` → `PUT` (streamed from the staged file, `application/octet-stream`) → `complete` → the link is written to the staged record (`OutgoingAttachmentStore`) → the message goes out. The start line logs `mode=`, `parts=` and `part_bytes=`. **Multipart**: parts go one after another, part n from offset `(n-1)·part_bytes` of the start answer (never a size of the client's own), each PUT read from the staged file at that offset (`HttpClient::PutFileRange`); `files/part`'s size must equal the planned one. A part whose `files/part` or PUT gets no answer or a 5xx, or whose PUT gets a 403 (expired URL), is tried again with a fresh `files/part` at most twice; `complete` is tried again at most twice on no answer, a 5xx or `UPLOAD_INCOMPLETE`. A cancel (the progress callback says stop) or any other refusal stops at once; progress is the part's offset plus its bytes sent and never goes back on a retry. No resume across sends (`files/status` is not used). After `start` has answered, **any** failure (the PUT or a part after its tries, `complete`, `BLOB_NOT_PENDING`, an unusable plan, the record write) deletes that `blob_id` (best effort) and reports the original error; the person resends. A `start` refused with `QUOTA_PENDING_COUNT` deletes this account's pending uploads created more than an hour ago (never newer ones: a PUT started in time may still be running, another device may be uploading, clocks drift) and starts once more. Download (`AttachmentFetch`): the only caller with `follow_redirects` (at most 2 hops, https only, public addresses on every hop, only the final body kept and capped); a download that got any answer from the relay, or whose bytes then could not be stored, is suppressed (`AttachmentSuppressionStore`) so automatic re-queues never GET it again, while a failure before any answer (offline, DNS, connect, TLS, timeout) spent nothing and may retry by itself; the card's Download / Retry asks once more.

Failure codes (`{ error, code }`; `ClassifyBlobFailure` takes the `code` first, the log detail is `HTTP <status> <CODE>: <text>`):

| Code (status) | `Err::Blob` | Client does |
|---|---|---|
| `FILE_TOO_LARGE` (400) | `TooLarge` | not retried |
| `QUOTA_FILE_BYTES` (429) | `QuotaExceeded` (tier `Files`) | `BlobQuotaRecoveryFlow`: `PlanOldestRelayFileDeletion` lists complete files and picks the oldest by `completed_at` until this file fits (the size the refused start asked for, else the 50 MB send cap plus the AEAD tag), deleting each on `files/delete` (`DeleteRelayQuotaItem` routes a plan's ids by its tier; profile-photo plans stay on `/blobs/delete`) (of the newest 500; if all of them would not free enough, "cloud space is full" and nothing is deleted); after the confirm, deletes them and uploads once more; a second full answer is final |
| `QUOTA_PENDING_COUNT` (429) | `RateLimited` | the stale-pending clean-up above, once |
| `BLOB_NOT_FOUND` (404); a 404 download | `Expired` | "Attachment expired."; a delete of a copy already gone is fine |
| `BLOB_NOT_OWNED` (403) | `UploadRejected` | failure line |
| `UPLOAD_NOT_CONFIGURED` (503) | `UploadNotConfigured` | "server hasn't enabled uploads" (`UploadNotConfiguredNoDirect` after a failed direct push) |
| `BLOB_NOT_PENDING`, `INVALID_BYTE_LENGTH`, `UPLOAD_INCOMPLETE` (400) | `Generic` | failure line (after start: the blob is deleted) |

"Delete for everyone" succeeds once the retract is sent and the message is deleted here; deleting our server copy by the link's blob id (`DeleteUploadedAttachmentCopy`, `files/delete`) is a separate best-effort step whose failure is logged (WARNING, blob id and the delete's own error); an old CDN link has nothing to delete and is skipped (one INFO line). "Delete for me", clearing history and deleting a thread do not touch the server copy (it expires 7 days after `complete`).

## HTTP account lockers (multi-device M023)

Same base path as the blobs. Client: `HttpLockerClient` / `ILockerClient`; sealed and checked by `lib/people/AccountLocker` ([M023](../../projects/multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) §4–5). The relay is one locker provider among several: it cannot read a snapshot, and keeps only the newest version.

| HTTP | Purpose |
|------|---------|
| `POST /v1/lockers/put` | `{ relay_user_id, timestamp, signature, locker: { locker_id, version, signing_public_key_b64, sealed_b64, signature_b64 } }` → `200 { locker_id, version }`; **409** when the stored version is the same or newer (also when two writers race); **403** `Locker quota reached` past 4 lockers per relay user; body ≤ 2 MiB |
| `GET /v1/lockers/:locker_id` | `200 { locker: {…} }` or **404**. By id alone — a restoring device is not registered yet; rate-limited per IP |

Two signatures on a write:

| Half | Signed by | Bytes |
|------|-----------|-------|
| Relay (quota) | the relay user's account key | `pp-browser:relay-locker-put-v1` ‖ 0 ‖ u8 1 ‖ LenUtf8 `relay_user_id` ‖ LenUtf8 `locker_id` ‖ u64 `version` ‖ i64 `timestamp` |
| Locker (owner) | the locker ML-DSA-65 key; `locker_id` = lowercase hex of BLAKE2b-256(its public key) | `pp-browser:account-locker-v1` ‖ 0 ‖ u8 1 ‖ LenBytes(locker id) ‖ u64 `version` ‖ LenBytes(sealed) |

The snapshot (`sealed`, the owner signature, rollback protection) and the state inside it: [ACCOUNT_LOCKER.md](ACCOUNT_LOCKER.md). Golden vectors for both sign-byte layouts are shared between `account_locker_test` / `http_locker_client_test` and www `__tests__/AccountLocker.test.ts`. The relay learns which registered user writes which locker (the write is authenticated for its quota); reads carry no identity.

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
| `crash_reports_url` | no | Absolute opt-in crash ingest URL, **https only** (anything else is ignored); empty → `{relay.base_url}/v1/crash-reports` |
| `support` | no | Optional Support discovery block (`enabled`, `account_id`, `display_name`) |

Unknown keys are ignored. Network failure: fail open (no gate); use last good profile cache (`client_compat.json`, TTL 6h) when present.

## HTTP crash reports (opt-in)

Unauthenticated public POST (same org backend family as client-compat; no vault unlock required). Client only sends when Me → Security **Send crash reports** is on.

`POST {relay.base_url}/v1/crash-reports`  
(or absolute https `crash_reports_url` from a fresh client-compat cache)

```json
{
  "schema_version": 1,
  "product": "pp-browser",
  "version": "1.0.0-rc14",
  "os": "linux",
  "reason": "signal 11",
  "fingerprint": "a1b2c3d4e5f60718",
  "frames": ["0x7fff…", "0x7ffe…"],
  "breadcrumb_tags": ["CallStack", "Application"],
  "sample": "truncated raw dump text"
}
```

| Field | Notes |
|-------|--------|
| `fingerprint` | Client FNV-1a of `version\|reason\|frames[]` for server clustering / dedupe |
| `frames` | Raw addresses only (no symbols); capped |
| `breadcrumb_tags` | Logger component names only — not chat content |
| `sample` | Truncated local dump for triage: metadata and frames only, cut before the breadcrumb lines |

Server should cluster by `fingerprint` (+ version), keep a sample, increment counts. LLM summarization (if any) belongs server-side on new/growing clusters — not on the client crash path.

## HTTP device push (opaque wake)

Signed with `pp-browser:relay-api-v1` ops `DeviceRegister=3` / `DeviceUnregister=4` over `(relay_user_id, platform, device_id, push_token)`.

| HTTP | Purpose |
|------|---------|
| `POST /v1/devices/register` | Bind push token `{ platform, device_id, push_token, relay_user_id, timestamp, signature }` (`platform` is `android` or `ios`) |
| `POST /v1/devices/unregister` | Remove device binding (the client keeps its binding with alerts off — P005 amended; account revoke removes all of them) |

On `POST /v1/messages` accept, a conforming relay **best-effort** wakes registered devices for the recipient:

| Condition | Opaque type |
|-----------|-------------|
| Accepted message | `inbox_wake` |
| Accepted message with `"urgent": true`, within the urgent rate limit | `call_wake` |

The relay cannot tell a call invite from a message (the control type is inside the end-to-end payload). The sender says so instead: a call invite's send record carries `"urgent": true` (P012). The hint sits next to the record's fields and is **not** in the signed bytes, so a relay that does not know it ignores it and verifies the request as before. The relay trusts it no further than a rate limit: per sender → recipient, at most one urgent wake per 15 s; over the limit the message wakes as `inbox_wake`. Every wake syncs the inbox, a call invite in it rings, and every ring raises the "Incoming call" notification where the user is not looking (P010 / `CallPresenter`).

| `platform` | Provider shape |
|------------|----------------|
| `android` | FCM **data** message `{ "type": "…" }` |
| `ios` | `inbox_wake`: APNs silent push (`content-available: 1`, `apns-push-type: background`, priority 5) with custom `type`. `call_wake`: a visible alert (`apns-push-type: alert`, priority 10, `apns-collapse-id: pp-call`, `aps.interruption-level: time-sensitive`, `aps.sound: default`, `aps.alert: { title-loc-key: CALL_INCOMING_TITLE, loc-key: CALL_INCOMING_BODY }` — worded on the device from `packaging/ios/*.lproj`, nothing about the call in the push) with `type: call_wake`, because iOS throttles silent pushes and does not wake an app the user swiped away. The collapse id is the app's own incoming-call banner id, so the two replace each other |
| `ios_voip` | PushKit token of a CallKit build (P013), `device_id` = its `ios` device id + `.voip`. Calls only: `call_wake` as a VoIP push (`apns-push-type: voip`, priority 10, topic `<bundle id>.voip`, `{ "type": "call_wake" }`), which then replaces that phone's `ios` call alert. Never anything else — iOS requires the app to report a CallKit call for every VoIP push |

Payloads remain **opaque** — no `call_id`, names, thread ids, or media. Failures must not fail message store. How the provider authenticates to FCM/APNs is outside this contract. See [projects/push-notifications](../../projects/push-notifications/) and [p2p-av-calls V006](../../projects/p2p-av-calls/DECISIONS.md).

## Native agent tools

[`MessagingTools`](../../src/feature/product/MessagingTools.cpp) exposes `search_people`, `register_user`, and `update_profile_nickname` as native C++ tools calling `ConversationsHub` → `Http*Client` directly (not via MCP).

## MCP client buckets

| Bucket | Config | Agent tools |
|--------|--------|-------------|
| Promoted | `promoted_mcp` (+ platform default) | Feed/AI tools (not relay infra) |
| Custom | `mcp_servers[]` | All tools; collisions prefixed as `{id}__{tool}` |

## Brief LLM gateway

Default assistant preset uses OpenAI-compatible completions at:

`POST https://backend.example.test/api/llm/v1/chat/completions`

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

### Streamed answers

`POST https://backend.example.test/api/llm/v1/pp/chat/stream` — JSON request (`message`, optional `intent`, `context.history` / `summary`, `client.capabilities`; up to 6 MB), `text/event-stream` response (`data: {"type": meta|status|token|done|handoff|error}` … `data: [DONE]`, `: ping` heartbeat every 10 s of silence). Same Bearer, limits and error shapes as the completions endpoint; one streamed answer counts as one request. Stopping is closing the connection: www aborts its upstream request and brief_AI stops generating. The wire contract is owned by brief_AI (`docs/contracts/pp-client.md` in that repo, v0.3); www relays the body unchanged and adds no user id. `PP_BROWSER_BRIEF_STREAM_URL` (dev only) redirects the client to a local fake server; the Bearer goes to an override only over https or loopback.

## libp2p (deferred)

Future work adds `Libp2p*Client` implementations behind the same interfaces and sign-byte auth over libp2p HTTP to www.
