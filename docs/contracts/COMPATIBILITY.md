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
| Flat `threads/index.json` / `threads/*.json` (before the SQLite store) | Left on disk; never opened |
| Leftover plaintext `identity.json` | Ignored — only `identity.enc` is used (no migrator; wipe profile if upgrading from plaintext era) |
| `identity.enc` without its ML-KEM-768 account and device keys or its ML-DSA-65 account key, or an Account ID that is not that key's (a profile from before them) | Unlock **error** — keys are minted only when the profile is created, never on load; wipe the profile |
| `contacts.json` of another `schema_version` than this build's (older, newer, or none) | Contacts **error** (the store refuses the file); wipe the profile data |
| A contact row without `local` (the flat form before the local/remote split) | Only its id is read |
| Config keys from before a rename (`libp2p`, `mcp`) | Ignored (read `mesh`, `promoted_mcp`); a config without `backend` is on the default backend whatever URLs it carries, and one without `llm.preset` is on "brief" |
| Orphan `threads/{id}/` with `thread.db` | Re-cataloged as AI threads |
| Corrupt / **newer** `manifest` / prefs / `profiles.json` / `config_version` | Bootstrap **error** → process exit 1 (not a crash) |
| Wrong `vault.bin` magic/version; bad `identity.enc` | Unlock **error** |
| SQLite `user_version` | Wipe-out: CREATE TABLE at the current schema (`profile.db` **4** includes encrypted `preview_enc`; `thread.db` **2** uses `content_enc` / `value_enc` — D102). Leftover older DBs are not a supported upgrade — clear them with Me → Storage → **Reset chats** (`{profile}/threads/`; the account, contacts and settings stay) or **Reset my data** (the whole profile dir). Additive `ALTER` may still run locally as a convenience, not a dual-parser promise. |

**Policy:** unknown junk is safe. Pre-release there is no leftover-data handling ([§ No bridges](#no-bridges-pre-release)): an official file of an older form is refused or ignored like a newer one, never upgraded (the additive SQLite `ALTER`s above are the one exception left). Official filenames with unsupported **newer** schemas fail bootstrap until the profile/data dir is reset (Me → Storage, or delete data dir in development); an unsupported chat-store schema (`profile.db` / `thread.db`) fails each chat operation, and is logged at startup, until Me → Storage → Reset chats. See D016 (no legacy JSON/wire import) vs D069 (migrate shippable SQLite).

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
| Sign v4 — group copies name their group only inside their ciphertext, group stream ids are a hash of the group and the pair ([WIRE_SCHEMAS.md § Group](WIRE_SCHEMAS.md), group-chat G018 item 7) | Every envelope's signature changes (direct and control too), so messages, group copies and call signaling between apps before and after it are refused — as a signature that does not verify (`envelope_version` stayed 2: the receiver cannot tell an older sender from a forgery; a `control.unverified reason=signature_mismatch` line, [DIAGNOSTICS.md](DIAGNOSTICS.md)); an envelope whose route names a group is refused. Group history stored on the relay under the earlier stream ids is not fetched again. No relay change (messenger treats stream ids as opaque). |
| Amp wire v2 — sealed packets, an 8 B association id, a 1232 B datagram ([ADP.md](ADP.md), [A029](../../projects/adp/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H1; pp-cpp-amp v4.0.0) | No Amp link between an app or node before it and one after it: each refuses the other's packets (version byte), so chat, sync, calls and the mesh itself need both ends updated. Apps and pp-node ship it together. |
| One media frame, an 18 B header and a derived nonce ([WIRE_SCHEMAS § Media frames](WIRE_SCHEMAS.md#media-frames), [A029](../../projects/adp/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H2; pp-cpp-mesh v5.0.0) | No call or broadcast media between an app before it and one after it: each refuses the other's frames (version), and a hop before it drops them. Apps and pp-node ship it with amp wire v2. |
| Addresses say their wire version, `/adp/2/` ([ADP.md § Addresses](ADP.md#addresses), [N032](../../projects/p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes) item 3; pp-cpp-amp v5.0.0) | An app or node on pp-cpp-amp before v5.0.0 cannot parse an address written after it, so it cannot dial it. An address of another wire version is skipped before dialing (the versionless `/adp/1.0.0/` too since the row below). Ships with amp wire v2. |
| Call audio in 40 ms Opus packets ([CALLS.md § Audio packets](../architecture/CALLS.md), [A029](../../projects/adp/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) step 1) | An app before it hears nothing from an app after it: it decodes every packet into a 20 ms buffer, which a 40 ms packet does not fit. An app after it plays both, so it hears an older peer. Calls, group calls and broadcasts alike. |
| Only versioned addresses parse: `/adp/1.0.0/` is refused ([ADP.md § Addresses](ADP.md#addresses), 2026-11-12, no bridges) | A contact's, a config's or a seed list's address stored in the versionless form is skipped like an unparsable one: re-add the contact or fix the config. An app or node before it still writes and dials `/adp/2/`, so the two reach each other at every current address. |
| LAN discovery says its wire version: mDNS TXT `wire=<N>` beside `peer=` ([ADP.md § Addresses](ADP.md#addresses), 2026-11-12) | An app after it does not hear one before it on the LAN (its announcement says no version, so it is not a browse result); one before it hears one after it as before. A same-LAN peer is still reached by every other path. |
| Channel 0's capability offer says what the node runs: leading byte `4`, its wire version, node version and features inside it, no separate extension ([AMP-CHANNEL § Channel 0](AMP-CHANNEL.md#channel-0--capability-plane), N032; 2026-11-12) | A link between an app or node before it and one after it comes up, but neither takes the other's offer (`1` and `4` are each refused by the other's decoder): no listen addresses or protocols are learned over the link, and the node's claims are not seen, so they cannot exclude it. |
| Nested links always speak the reliable lane, and send every handshake message in pieces of one outer frame ([AMP-CHANNEL § Carrier payloads](AMP-CHANNEL.md), 2026-11-12) | A nested link (through a relay) between an app or node before it and one after it still works: the newer side sends no lane probes and wraps reliable frames in `LaneData` from the start, and the older side takes the lane up on the first lane frame it gets (until then the newer side reads its plain sealed frames). A large handshake message now goes in pieces on its first send, which the older side already reads (its resends came so). |
| A channel OPEN carries its max message bytes (2026-11-12) | An OPEN without the trailing u32 is refused; every build writes it, so no current peer is affected. |
| Circuit and punch refusals by `code` only ([pp-cpp-mesh docs/ERRORS.md](https://github.com/people-post/pp-cpp-mesh/blob/develop/docs/ERRORS.md), 2026-11-12) | A refusal with no code (a relay or introducer before codes) is a plain failure on an app after it: no retry of the not-reserved case, no busy / scope handling. Every current node sends codes. |
| The DHT: a third party's record without its `public_key_b64` counts against its server ([MESH_DHT.md](MESH_DHT.md), 2026-11-12) | A node before records carried the key serves them without it: a node after it drops them and lowers that server's soft reputation. Every current node stores and serves the key. |
| The M024 bridges dropped: the control lane goes per device whenever the sender holds the peer's card (single-device accounts too), and a call's media key goes only as `media_key_b64` inside the lane copy — `wrapped_key_b64` is neither sent nor read ([M024](../../projects/multi-device-account/DECISIONS.md#m024--the-control-lane-goes-per-device-a-calls-media-key-rides-the-copy), [WIRE_SCHEMAS § Call media keys](WIRE_SCHEMAS.md), [P2P_MESSAGING § The control lane](../architecture/P2P_MESSAGING.md#the-control-lane)) | An app before M024 refuses every lane record from an app after it whose sender holds its card (device copies): calls do not ring there, and card and copy asks go unanswered; it also finds no key it reads (`wrapped_key_b64`) in an invite or `call_media_key`. An app after it takes no key from an app before M024, which sends only `wrapped_key_b64`: the call gets no media key. A first contact (no card held) still goes as the account copy both ways. An app on the bridge release and one after it work together: it sends and reads both device copies and `media_key_b64`. |
| Calls: `call_sdp`, `call_ice` and the `call_join` alias are no longer known control types, and an invite without `video_allowed` does not allow video ([CALLS.md § Inbound call control](../architecture/CALLS.md)) | Nothing has sent `call_sdp` / `call_ice` since calls moved to Amp, and `call_join` was the accept's former name: an app after it ignores all three like any unknown control type, so a `call_join` from a very old peer does not answer the call. An invite from an app before V035 (`media_mode: "video"` without `video_allowed`) rings with video not allowed; every app since V035 sends `video_allowed`. |
| Call rows lose their kind: the legacy `Broadcast` session kind is gone ([media-client-layers l6](../../projects/media-client-layers/PHASES.md)) | An app after it neither reads nor writes `session_kind`. A leftover `profile.db` row written as a broadcast by a build before l6 would load as an ordinary call row, but leftover DBs are not a supported upgrade (§ On-disk). No wire change. |

**Changing what is signed.** A change to an envelope's signed bytes (`EnvelopeSigner`: its sign domain, sign
version, or the fields it covers) also changes something a receiver checks **before** it verifies — the
`envelope_version`, or a sign version carried and checked as such — so an app on the other side of it refuses the
envelope as an unsupported version, not as a signature that does not verify. A silent mismatch looks like a
forgery: nothing says "update", and the receiver cannot tell the two apart. Sign v4 (2026-11-06, group-chat G018
item 7) changed the domain and the version byte inside the signed bytes but not `envelope_version`, and a peer
on a build before it showed only as failed signature checks (#555). No fallback to the older signed form: crypto
stays strict. The signed bytes are frozen in a test (`EnvelopeSignerTest.SignBytesMatchFrozenHash`, beside
`kRelayEnvelopeVersion`): a change that moves its hash changes a version receivers check in the same change.

### No bridges (pre-release)

Pre-release every app and node updates together (the user's decision, 2026-11-12), so no code exists to talk to an
older peer, read an older wire form, or read older data on disk: a change is a clean cutover, recorded as a row in
[§ Clean cutovers](#clean-cutovers-pre-release) (what an app or node before it and one after it do to each other).
What stays is policy, not a bridge: unknown additive keys are ignored, an unknown or unsupported version is refused
cleanly (no crash; [§ Change classes](#change-classes)), and crypto stays strict. Old input is refused or ignored,
never upgraded and never a crash. (The M024 bridges, the only ones kept for a release, were removed on 2026-11-12:
see their rows above.)

### HTTP / mesh protocols

- Prefer **additive** JSON on existing `/v1/…` responses; old clients ignore unknown fields.
- Breaking HTTP: new path or explicit API version; keep `/v1` until sunset.
- **Client-compat discovery:** unauthenticated `GET /v1/client-compat` on the relay base URL ([SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md#client-compatibility-discovery)). Compare local app semver (`PP_BROWSER_RELEASE_VERSION`) to `min_client_version` / `latest_client_version`. Below min → blocking update dialog; below latest → dismissible banner. Fail open on network error; cache TTL 6h under the profile dir. This is a **relay/service** floor only.
- **Peer mismatch:** discovered via messaging / libp2p (soft-skip unknown wire, protocol-id negotiation failure)—not via directory profile fields. Optional future: in-band capability hello on the chat protocol.
- Mesh protocols: version in protocol id (e.g. `/pp/rpc/1.0.0`); a bump serves the new id only (pre-release, no bridges); no mutual protocol → clear incompatible state (the open is refused, `kOpenRejectedNoHandler`), not a crash loop.

---

## Version axes (quick)

| Artifact | Field | Notes |
|----------|--------|-------|
| App release | `PP_BROWSER_RELEASE_VERSION` / `AppVersionString()` | Store / About / `min_client_version` compare |
| Protocol gen | `kProtocolGen` | App constant; reserved for future in-band messaging hello (not directory) |
| Amp wire | the packet's first nibble (`adp::kWireVersion`) | A mismatch answered by version negotiation (`0xF`, [ADP.md](ADP.md)): the dial fails at once (N032) |
| Mesh node | `amp_wire` / `node_version` / `features` | A node's own claims in its directory row and its capability offer on channel 0 (N032): they exclude a node, never rank it higher. A directory row that says nothing (the directory is a relay service, not an app or node) is dialed as before |
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
