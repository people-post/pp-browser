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
| SQLite `user_version` | Wipe-out: CREATE TABLE at the current schema (`profile.db` **4** includes encrypted `preview_enc`; `thread.db` **2** uses `content_enc` / `value_enc` — D102). Leftover older DBs are not a supported upgrade — clear them with Me → Storage → **Reset chats** (`{profile}/threads/`; the account, contacts and settings stay) or **Reset my data** (the whole profile dir). Additive `ALTER` may still run locally as a convenience, not a dual-parser promise. |

**Policy:** unknown junk is safe. Official filenames with unsupported **newer** schemas fail bootstrap until the profile/data dir is reset (Me → Storage, or delete data dir in development); an unsupported chat-store schema (`profile.db` / `thread.db`) fails each chat operation, and is logged at startup, until Me → Storage → Reset chats. See D016 (no legacy JSON/wire import) vs D069 (migrate shippable SQLite).

### Dev wipe after Brief PQ / Account ID hard cut

When Brief drops classical `relay_users` (ML-DSA-only register), **or** after the client **m2b** wire cut (`PeerSessionKey` / envelopes → Account ID), **or** after the **libp2p-pq-transport** device-key hard cut (`identity.enc` schema **3** — ML-DSA-65 PeerId + `/noise-mlkem768/1.0.0`), wipe local profiles so clients re-mint keys, re-register, and rebuild PeerId/endpoints:

| Platform | Script |
|----------|--------|
| Linux / macOS | [`scripts/dev/wipe_local_profile.sh`](../../scripts/dev/wipe_local_profile.sh) |
| Windows | [`scripts/dev/wipe_local_profile.ps1`](../../scripts/dev/wipe_local_profile.ps1) |
| Android / iOS | Clear app storage by hand (no script) |

Brief-side wipe: www repo `scripts/wipe_relay_pq.sh`.

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

### Clean cutovers (pre-release)

Taken whole, with no old/new bridge: every app updates.

| Change | Old ↔ new |
|--------|-----------|
| Account card v2 — the device list with each device's keys ([ACCOUNT_CARD.md](ACCOUNT_CARD.md), M021 d1) | Cards between apps before and after it are refused (card exchange over Amp, contact cards, roster cards): a contact added across the line has keys only once both update. |
| Envelope v2 / sign v3 — one record per account with a copy per device, device signatures, sessions per device pair ([WIRE_SCHEMAS.md](WIRE_SCHEMAS.md), M021 d2 / M022) | Messages, group copies and call signaling between apps before and after it are refused (unknown `envelope_version`). Sessions with a peer's devices start fresh; account-level sessions are kept. Needs the relay that accepts an account's records to itself (messenger, M022) — deployed first. |
| Amp wire v2 — sealed packets, an 8 B association id, a 1232 B datagram ([ADP.md](ADP.md), [A029](../../projects/adp/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H1; pp-cpp-amp v4.0.0) | No Amp link between an app or node before it and one after it: each refuses the other's packets (version byte), so chat, sync, calls and the mesh itself need both ends updated. Apps and pp-node ship it together. |
| One media frame, an 18 B header and a derived nonce ([WIRE_SCHEMAS § Media frames](WIRE_SCHEMAS.md#media-frames), [A029](../../projects/adp/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H2; pp-cpp-mesh v5.0.0) | No call or broadcast media between an app before it and one after it: each refuses the other's frames (version), and a hop before it drops them. Apps and pp-node ship it with amp wire v2. |
| Addresses say their wire version, `/adp/2/` ([ADP.md § Addresses](ADP.md#addresses), [N032](../../projects/p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes) item 3; pp-cpp-amp v5.0.0) | An app or node on pp-cpp-amp before v5.0.0 cannot parse an address written after it, so it cannot dial it. After it, a legacy `/adp/1.0.0/` address (a contact's, a config's seeds) still dials, and an address of another wire version is skipped before dialing. Ships with amp wire v2. |
| Call audio in 40 ms Opus packets ([CALLS.md § Audio packets](../architecture/CALLS.md), [A029](../../projects/adp/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) step 1) | An app before it hears nothing from an app after it: it decodes every packet into a 20 ms buffer, which a 40 ms packet does not fit. An app after it plays both, so it hears an older peer. Calls, group calls and broadcasts alike. |

### HTTP / libp2p

- Prefer **additive** JSON on existing `/v1/…` responses; old clients ignore unknown fields.
- Breaking HTTP: new path or explicit API version; keep `/v1` until sunset.
- **Client-compat discovery:** unauthenticated `GET /v1/client-compat` on the relay base URL ([SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md#client-compatibility-discovery)). Compare local app semver (`PP_BROWSER_RELEASE_VERSION`) to `min_client_version` / `latest_client_version`. Below min → blocking update dialog; below latest → dismissible banner. Fail open on network error; cache TTL 6h under the profile dir. This is a **relay/service** floor only.
- **Peer mismatch:** discovered via messaging / libp2p (soft-skip unknown wire, protocol-id negotiation failure)—not via directory profile fields. Optional future: in-band capability hello on the chat protocol.
- libp2p: version in protocol id (e.g. `/pp/rpc/1.0.0`); advertise multiple ids when bumping; no mutual protocol → clear incompatible state, not a crash loop.

---

## Version axes (quick)

| Artifact | Field | Notes |
|----------|--------|-------|
| App release | `PP_BROWSER_RELEASE_VERSION` / `AppVersionString()` | Store / About / `min_client_version` compare |
| Protocol gen | `kProtocolGen` | App constant; reserved for future in-band messaging hello (not directory) |
| Amp wire | the packet's first nibble (`adp::kWireVersion`) | A mismatch answered by version negotiation (`0xF`, [ADP.md](ADP.md)): the dial fails at once (N032) |
| Mesh node | `amp_wire` / `node_version` / `features` | A node's own claims in its directory row and channel 0's extension (N032): they exclude a node, never rank it higher. Additive: a node or directory before them says nothing and is dialed as before |
| Client-compat doc | `schema_version` on `/v1/client-compat` | Product / relay upgrade gate |
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
