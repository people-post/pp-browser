# Compatibility policy

**Tier:** contract

**Status:** Normative policy (contracts tier).  
**Related:** [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md), [DATA_LAYOUT.md](DATA_LAYOUT.md), [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md), [AT_REST_ENCRYPTION.md](AT_REST_ENCRYPTION.md), [docs/README.md](../README.md).

This doc answers: what happens when the data directory has leftovers, or when a **newer peer / API** talks to this build? Goal: **no crash**; degrade or fail clearly; avoid dual parsers for obsolete wire (D016).

---

## Change classes

| Class | Examples | Older client / dirty disk |
|-------|----------|---------------------------|
| **Additive** | Extra envelope JSON keys; extra keys inside a known `content_type` payload; unknown HTTP response fields | **Ignore** and continue |
| **Soft semantic** | New `content_type`; optional new route kind the client does not implement | **Soft-skip** or placeholder — do not exit the process |
| **Hard protocol** | New `envelope_version` / AAD / AEAD blob / signed canonical set; corrupt vault; unsupported **newer** local `schema_version` | **Reject** that unit (message or bootstrap); crypto stays strict |

Do not use one policy for all three.

---

## On-disk (dirty folders)

Expected layout: [DATA_LAYOUT.md](DATA_LAYOUT.md).

| Dirt | Behavior today |
|------|----------------|
| Extra unrelated files/dirs under data or profile | Ignored |
| Legacy `threads/index.json` (+ flat `*.json`) | Wiped once (`SqliteThreadStore::WipeLegacyJsonIfPresent`) |
| Flat `threads/*.json` **without** `index.json` | Left on disk; never opened |
| Leftover plaintext `identity.json` | Ignored — only `identity.enc` is used (no migrator; wipe profile if upgrading from plaintext era) |
| Orphan `threads/{id}/` with `thread.db` | Re-cataloged as AI threads |
| Corrupt / **newer** `manifest` / prefs / `profiles.json` / `config_version` | Bootstrap **error** → process exit 1 (not a crash) |
| Wrong `vault.bin` magic/version; bad `identity.enc` | Unlock **error** |
| SQLite `user_version` | Create schema at v1; production bumps must **migrate** (D069) — not wipe |

**Policy:** unknown junk is safe. Official filenames with unsupported **newer** schemas fail bootstrap until the profile/data dir is reset (Me → Storage, or delete data dir in development). See D016 (no legacy JSON/wire import) vs D069 (migrate shippable SQLite).

---

## Wire / peer (newer apps)

Normative shapes: [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md). Crypto: [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md).

### Already decided (D073)

| Layer | Rule |
|-------|------|
| `RelayEnvelope` JSON | Ignore unknown **top-level** keys after required fields parse |
| Known `content_type` payload | Ignore unknown keys inside typed payload (when applicable) |
| `ChatHistoryRequest` / `Response` | Reject unknown top-level keys |
| Signature / AAD | Unknown envelope keys are **not** signed; AEAD/sign failures are **hard** |

### Target behavior (forward-compat)

| Condition | Desired outcome |
|-----------|-----------------|
| `envelope_version` == known | Full ingest |
| `envelope_version` > known (after JSON parse of required fields) | Soft-skip message; log; do not crash; prefer seq-safe handling so sync does not stall |
| Unknown `content_type` after successful decrypt | Soft-skip or placeholder bubble (“unsupported — update to view”); prefer consuming `sender_seq` |
| `payload_version` > known but outer crypto OK | Same as unknown content (soft) |
| Decrypt / verify / AEAD version failure | Hard reject that message (trust boundary) |
| Legacy shapes (`thread_id`, `body.text`, `content_b64`, `public_relay`) | Hard reject (D016 / D090) — not “newer”, obsolete |

**Implementation note:** ingest today often **hard-rejects** unknown `content_type` and exact-mismatched `envelope_version`. Soft-skip / placeholder is the intended direction for **newer** peers; keep hard rejects for crypto and known-legacy shapes.

### HTTP / libp2p

- Prefer **additive** JSON on existing `/v1/…` responses; old clients ignore unknown fields.
- Breaking HTTP: new path or explicit API version; keep `/v1` until sunset.
- **Client-compat discovery:** unauthenticated `GET /v1/client-compat` on the relay base URL ([SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md#client-compatibility-discovery)). Compare local app semver (`PP_BROWSER_RELEASE_VERSION`) to `min_client_version` / `latest_client_version`. Below min → blocking update dialog; below latest → dismissible banner. Fail open on network error; cache TTL 6h under the profile dir.
- **Peer protocol_gen:** directory lookup may return `app_version` / `protocol_gen` / `min_peer_protocol_gen`. If local `kProtocolGen` < peer `min_peer_protocol_gen` → contact “update required” banner (compose disabled). If peer gen < local `kMinPeerProtocolGen` → soft “peer outdated” banner. Absent fields default to **1**.
- libp2p: version in protocol id (e.g. `/pp-browser/chat-history/1.0.0`); advertise multiple ids when bumping; no mutual protocol → clear incompatible state, not a crash loop.

---

## Version axes (quick)

| Artifact | Field | Notes |
|----------|--------|-------|
| App release | `PP_BROWSER_RELEASE_VERSION` / `AppVersionString()` | Store / About / `min_client_version` compare |
| Protocol gen | `kProtocolGen` / `protocol_gen` | Peer capability floor (integer) |
| Client-compat doc | `schema_version` on `/v1/client-compat` | Product upgrade gate |
| SQLite | `PRAGMA user_version` | Migrate in production (D069) |
| Envelope | `envelope_version` | Signing / outer shape |
| ChatPayload | `payload_version` | Body binary layout |
| E2E AAD | `aad_version` | Crypto associated data |
| Vault | vault file version | At-rest |
| JSON stores | `schema_version` / `config_version` | Reject unsupported **newer** at load |

Full matrix: [WIRE_SCHEMAS § Versioning](WIRE_SCHEMAS.md#versioning-matrix).

---

## What we will not do

- Dual full parsers for every historical wire layout (D016).
- Soft-ignore AEAD or signature failures.
- Silent migration from plaintext `identity.json`.
- Treating project `DECISIONS.md` as the runtime source of truth once a contract doc exists — promote outcomes here / into wire & crypto docs; leave rationale in ADRs.
