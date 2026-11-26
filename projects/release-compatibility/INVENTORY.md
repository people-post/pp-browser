# Release compatibility: P0 inventory

**As of:** 2026-11-19, read at these exact commits (no repo had an open PR that day):

| Repo | Commit |
|------|--------|
| pp-browser | develop `76fd84209` |
| pp-relay-apis | develop `39ad307` (prod runs main `845a6cd`; the 4 commits between them change no format) |
| pp-cpp-amp | `v6.0.0` (`edf35d7`), as pp-browser pins it |
| pp-cpp-mesh | `v9.0.0` (`0ee46aa`) |
| pp-cpp-crypto | `v1.3.1` (`870bee9`) |
| pp-cpp-common | `v1.4.0` (`5a1d80e`) |

The libraries' develop branches hold no format change beyond those tags (the one pp-cpp-mesh commit, `10d6f07`, makes
the client send an `unsubscribe` the v9 hop already serves).

This is phase P0 of [PHASES.md](PHASES.md): every format in [DESIGN § Scope](DESIGN.md#scope), what it does today with
a version or field it does not know, and the gap to [DESIGN § Release 1](DESIGN.md#release-1) as work items. Paths
are relative to each repo; `pb`, `relay`, `amp`, `mesh`, `crypto` and `common` name the repos. Once the work list
([§ 4](#4-release-1-work-list)) is agreed it defines the freeze (P1), and P3 builds it.

## 1. What matters most

1. **No receiver anywhere can tell "newer" from "broken".** A newer form fails as a forgery ("signature verify
   failed": grants, roster statements, the locker, broadcast join tickets), as "couldn't decrypt" (a higher
   `payload_version`, `sub_version` or `group_copy_version`), as a network failure (Amp's "the peer's link table is
   full"), or silently (media frames, app RPCs, unknown content types). DESIGN's rule that every version refusal
   names who updates holds only for the relay's `426`, and even there the app shows a dismissable alert that comes
   back with each poll.
2. **Several of these also stall or loop.** A higher `envelope_version` leaves the thread in Gap for good, with one
   useless history fetch per later message. An unknown content type is decoded as "update to view" but cannot be
   stored, so the message is rejected and its seq not consumed. An unknown group change kind makes roster catch-up
   ask every 30 s. A DHT node whose peers all refuse it republishes every 10 s indefinitely.
3. **Most signed objects carry their version only inside the signed bytes.** The envelope, account card and
   broadcast announce carry it outside too; grants, roster statements and changes, the locker record, the KEM
   binding and peer requests do not, and the DHT record writes one outside that no reader checks. No signed object has an extension area, and almost no key,
   signature, KEM or AEAD blob names its algorithm (the DHT record's `signature_alg` is the exception, and it is not
   signed).
4. **Nothing announces a range.** The channel-0 offer carries one wire version, one node version and empty features;
   mesh protocol versions are only id strings, learned by try-and-fail; the relay reports no release version (its
   packages are `0.0.0`), no ranges and no node floor; no relay call carries a User-Agent or version header, so
   versions in use cannot be counted.
5. **The release-1 floor readers would themselves block the range.** The app's `client-compat` reader refuses
   `schema_version > 1`, and its `relay-identity` reader requires exactly 1. Every phone keeps those readers for
   good.
6. **The relay's "legacy, removed after migration" routes are the ones today's app uses**: `GET /v1/search`,
   `blobs/*`, `files/*`, `keys/rotate` and `llm_api_key`. The clean start has to move the app first.
7. **Data another install reads is dropped on write.** The account locker's writers keep only the keys they know,
   so an older device in range erases a newer one's data; the relay normalizes a stored account card to the fields
   it knows, so no card field can be added without a relay change.
8. **Account deletion leaves data behind.** A revoke removes the registration's three items and purges push devices.
   The mailbox META item, a retained public icon, the quota counter, lockers, Support rows, and push rows of an expired
   registration stay forever. An offline reset is never retried, and a reset on one device ends the registration
   for the whole account.
9. **Three local stores break DATA_LAYOUT's "a newer file is refused, never overwritten"**: `own_card.enc`,
   `initiation_billing.json`, `payment_promises.json`. Moving the JSON floors to release 1 is not like moving the
   SQLite ones: below its floor `identity.enc` or `contacts.json` is a hard error that loses the account.

**Corrections to DESIGN.** "`ChatHistoryRequest` refuses unknown keys" is wrong for the code: its request and
response parsers ignore them (`pb MessagingJson.cpp:710-752, 822-899`); the claim came from `WIRE_SCHEMAS.md:61` and
`COMPATIBILITY.md:68`, which drifted. "Ingest often hard-refuses an unknown content type" is right in effect but
for another reason (§ 2.1). DESIGN § Release 1 is updated in the same change.

## 2. Findings by pair

### 2.1 App ↔ app

| Format | Version and where | Checked first? | Newer version today | Unknown field / type today | Ext. area | Alg. named | Frozen data |
|--------|-------------------|----------------|---------------------|----------------------------|-----------|------------|-------------|
| Envelope | `envelope_version` 3 in JSON and signed bytes; sign v5 inside only | yes, before verify | silent drop, thread **stalls** in Gap | top-level keys ignored (unsigned); some legacy keys and an unknown `route.kind` or channel refused | none | no | sign-bytes hash |
| ChatPayload / group copy | `payload_version` 1, `sub_version` 1 (contact card 3), blob byte 1, `group_copy_version` 2, inside the ciphertext | after decrypt | "couldn't decrypt", seq **stalls** | exact end: an appended field is "couldn't decrypt"; unknown content type: **cannot be stored**, stalls | none (only `text_spans` keeps unknown kinds) | no | vector A only; group copy bytes |
| Account card | `card_version` 4 in JSON and signed bytes | yes | card dropped, then `card_ask` every ≥10 min | ignored, unsigned, dropped on re-serialize | none | no (sizes only) | golden vector |
| Relay grant | `relay-grant-v1` + u8 1 inside only | nothing to check | reads as a forgery; resends hourly, asks every 10 min; sends fall back to stranger | ignored | none | no | sign bytes (not in the shared vectors) |
| Contact card / locator | `sub_version` 3; `pp-browser-contact-v1:` prefix and URI scheme | yes | "couldn't decrypt"; v2 locator is "no contact locator" and the OS does not route a v2 URI | locator keys ignored | n/a | via card | round trips |
| Control lane | envelope + system `sub_version` 1; no per-type version (call `caps.v` 1) | as envelope | as envelope | unknown control type ignored; unknown `detail` keys ignored | none | no | unit tests only |
| Group statements, changes, invites | signing tags only (`statement-v3`, `change-v1`, …); invite link none | no | reads as a forgery; membership DM stalls | unknown change kind: DM stalls, bundle skips it and catch-up **loops** every 30 s; unknown role or preset breaks the signature | none | no | change-id / changes-hash goldens |
| Attachment metadata | `sub_version` 1 inside the ciphertext; file ciphertext has no version byte | after decrypt | "couldn't decrypt", stalls | appended field refused; unknown mime kept | none | no | round trips |
| Account locker | sign `account-locker-v1` + u8 1 inside only; state JSON unversioned; file `format` string | file yes, record no | reads as tampering; the older device's sync stops; restore fails | ignored on read, **dropped on write** | none | no | sign-bytes golden |
| Chat history | protocol id; `peer-request-sign-v1` | n/a | new id: refused open | ignored (DESIGN wrong); new fields unsigned | none | no | none |
| Others | link-device `-v1`, PSK bundle `-v1`, broadcast announce `schema_version` 1 (checked first), annotations, `psk_rotate`, KEM binding `-v1` | mixed | bundles: hard refusal; announce: refused with text | unknown `annotation_type` kept as a badge; unknown `wrap_kind` stalls | none | link bundle only | link bundle |

Notes, with the code that decides each:

- **Envelope stall.** The poll skips an envelope it cannot read and moves its cursor past it
  (`pb HttpRelayClient.cpp:246-252`); history skips it too (`MessagingJson.cpp:861-876`). Every later message of that
  pair is AcceptGap and fires a repair (`MeshDeliveryOrchestrator.cpp:1001-1005, 1501-1524`) that gets the same
  record, and `ChatSyncWorkflow::PassesEmptyGapGuard` (`ChatSyncWorkflow.cpp:86-98`) never passes while later rows
  exist. Over Amp the parse fails before the ack, so the sender gets none (`AmpDirectChatTransport.cpp:77-84`).
  Groups have no gap repair, so the hole just stays.
- **Unknown content type.** `ChatPayloadCodec` decodes it as `Unsupported` (`ChatPayloadCodec.cpp:371-379`) and the
  renderer has the "Update the app to view this message." bubble (`TranscriptRml.cpp:876`), but `EncodeToRow` returns
  "Cannot encode unsupported ChatPayload" (`:330-331`), so `AppendMessage` fails and the message is a silent
  HardReject (`RelayReceivePipeline.cpp:2081-2083`). No test covers the path end to end. Sent copies from a sibling
  hit the same bug (`:1619-1622`).
- **Post-decrypt version refusals** are reported as `DecryptFailed` (`RelayReceivePipeline.cpp:1801-1805, 2196`), shown
  as "couldn't decrypt" (`en.json:1326`), and leave the seq unconsumed. Every type tail ends with `ReadExactEnd`
  (`ChatPayloadCodec.cpp:365, 397, 413, 422, 444, 461`); the text type reads no `format` tail, so the documented
  non-plain format (`WIRE_SCHEMAS.md:239-244`) would be refused as trailing bytes.
- **Other refusals after a good verify also stall:** the annotation cap (`RelayReceivePipeline.cpp:2049-2054`),
  membership apply (`:2037-2041`), `psk_rotate` with an unknown `wrap_kind` (`:1884-1889`).
- **Group roles and presets** read an unknown value as the default (`GroupTypes.cpp:24-33`) and rebuild the sign bytes
  from the enum (`GroupRosterStatement.cpp:50-58`), so a new role fails the signature. An unknown change kind in a
  bundle is skipped (`GroupRosterSyncCodec.cpp:86-97`), `changes_hash` never matches, and `NeedsCatchUp` asks every
  ≥30 s (`GroupRosterSync.cpp:789-794`). The invite link's maker field takes the rest of the bytes
  (`GroupInviteLink.cpp:65-103`), so nothing can be appended.
- **Locker.** `PickNewest` skips a newer record, Fetch fails with "does not verify" and Publish stops
  (`AccountLockerWorkflow.cpp:315-318, 467-472`): the older device never overwrites, but never syncs again either.
  Unknown keys are read (test `account_locker_test.cpp:263`) but `StateToJson`, `GroupToJson` and the contact rows
  write only known keys (`AccountLocker.cpp:131-184`, `AccountStateCodec.cpp:14-35`). That breaks RC015's rule for
  data another install reads.
- **Control types**, all on the lane unless noted: `device_list`, `sent_copy`, `card_ask`, `copy_ask`,
  `relay_grant` / `_ack` / `_ask`, 20 `call_*` kinds (`CallControlFamily.cpp:155-178`), `group_roster_*`,
  `group_join_*`; in the stream: billing (`charge_required`, `initiation_*`), promises (`promise_*`), `psk_rotate`
  and the membership types. An unknown type is a SilentDiscard on the lane (`RelayReceivePipeline.cpp:1305-1309`) and
  a plain system line in the stream. A new required call step on an older peer therefore fails silently.
- **Not found:** typing indicators and read receipts. Broadcast RPC ops and the broadcast media-key wrap were not read
  in depth.

### 2.2 App / pp-node ↔ relay

| Format / call | Version and where | Newer version today | Unknown field today | Unknown error code today | Retries | Alg. named | Frozen data |
|---------------|-------------------|---------------------|---------------------|--------------------------|---------|------------|-------------|
| Signed calls (register v3, relay-api-v1 v3, the rest v2) | `sign_version` in the body, read first by the relay | relay: older or absent → `426`, newer → `400 unsupported_sign_version`; app types only the `426` | relay ignores unknown keys; unknown body keys are never signed | app: a 4xx other than 408/429 is "refused", the rest transient | per call (below) | registration only | shared vectors, 31 calls |
| Card at registration | `card_version` 4 | v1–3 → `426`; v5 → generic `400 invalid_account_card` | relay **strips** unknown card keys before storing | app shows "status 400" | — | no | shared vectors (refused cards) |
| `client-compat` | `schema_version` 1 | app refuses > 1 and keeps its cached copy of any age (fails open only with none) | ignored by both | non-2xx: cached copy | at first paint, on foreground ≥15 min, on a `426` | — | relay unit tests |
| `relay-identity` | `schema_version` 1 | app: ≠ 1 is "unavailable" | ignored | — | once per run; after `wrong_audience` every ≥5 min | `ml-dsa-65` | relay pins tests |
| Inbox poll / send | relay-api-v1 v3 | `426` typed | tolerant; an unreadable envelope is skipped | send reads 7 codes, the rest by status | poll every 2 s whatever it gets; send ≤3 posts then ≤5 outbox retries | none | client tests |
| pp-node registration / heartbeat | register v3 + `node_version`, `amp_wire` (no `features`); heartbeat v2 without `node_version` | `426`: one log line, then quiet retries | — | heartbeat maps 401, 409, 404 ×3, 400/403 | registration 10 s doubling to 5 min; heartbeat every 5 min forever | registration only | heartbeat tests |
| Blob / file / locker | v2 sign; locker `version` is a revision | `426` typed | tolerant | blob: uppercase codes, then exact `error` text; locker: any 409 is NotNewer, quota by text | locker every 10 min | none | client tests |
| AI `/api/llm/v1` | token prefix (`ppl1` on the relay) | — | SSE: unknown events and keys dropped | 429 sub-kinds by **English message text**; only `server_busy` read as a code | `server_busy` once after 3 s | Ed25519 token (relay) | none |
| Push wakes | none in payloads | — | — | Android: an unknown `type` is silently ignored (no sync); iOS VoIP turns every push into a call | — | — | none |
| Support | rides the app ↔ app formats (envelope 3, sign v5, card v4) in the support-worker | worker drops an unreadable record and moves on; the customer is not told | — | — | — | — | local-stack test |

Notes:

- **Retiring the relay's legacy routes would break today's app.** The app calls only `/v1/blobs/*` and `/v1/files/*`
  (`pb HttpBlobClient.cpp:195-372`), `GET /v1/search`, `keys/rotate`, and reads `llm_api_key` from register/finish
  (`OrgBackendClientsImpl.cpp:594-597`); it never reads `llm_token` or calls `token/renew`. `RelayFileUrl::BlobIdOf`
  hard-codes `/api/relay/v1/files/<id>/download` (`RelayFileUrl.cpp:8-32`), and `MaxConcurrentTurnsFor` gives the
  registered limit only to keys starting `brf_llm_` (`AgentSessions.cpp:17-27`).
- **The relay's legacy window** (`relay 94c6865`, until 2026-12-31): a call naming neither `sign_version` nor
  `audience` is verified over the pre-audience layout (`parse.ts:77-91`, 45 references in 9 files). Other pre-release
  legacy: `llm_api_key` and `keys/rotate` (`llm-credentials.ts`), `blobs/*`, `files/*`, `GET /search`
  (`storage/src/routes.ts:44,49`; `registry/src/routes.ts:51,72`), the pre-cutover sender mapping (SPECS.md:22-25).
- **`426` in the app** goes `RelayPins::Refused` → `ConversationsHub::OnRelayRequiresUpgrade` →
  `ClientCompatController::OnOrgRelayRequiresUpgrade` (`ClientCompatController.cpp:97-157`), which shows an ordinary
  alert. With the poll at 2 s it comes back as soon as it is dismissed. A non-empty `doc.message` replaces the
  translated text with one server string (`:152`). For our other relays the My relays row says "Needs a newer
  version of the app"; for a contact's relay only the send error says so.
- **Version compare** drops the prerelease part (`1.0.0-rc16` equals `1.0.0`), and a malformed `min_client_version`
  never triggers an update (`ClientCompat.cpp:16-63, 120-148`).
- **Headers:** callers pass only `Content-Type`; `CURLOPT_USERAGENT` is set nowhere (`HttpClient.cpp:196-203,
  342-347, 503-508`). The app's version leaves the device in the AI stream body (`BriefAiClient.cpp:93-102`, not read
  by the relay), crash reports, and pp-node's `node_version` at registration. pp-node never fetches `client-compat`.
- **Relay identity** is expected, never learned: one compiled PeerId per environment (`DeploymentProfile.cpp:16-22,
  56-63`), and an answer naming another PeerId leaves the relay unused (`RelayPins.cpp:301-330`). The relay's key
  signs nothing today. Rotating the org relay's key would stop every shipped build.
- **Behaviour tests:** only `lab-nightly.yml` runs the app lab against the relay's local stack: nightly or manual, never
  on PRs, against relay `main`, idle until its variables and secret are set. The relay's `deploy` job needs `ci` only,
  not `integration` (`relay .github/workflows/ci.yml:72-76`). The only cross-repo check is the shared sign vectors,
  which cover no newer `sign_version`, no unknown body key and no response shape.
- **Crash reports:** the uploader exists (`pb CrashReportUpload.cpp:36-79`) but the relay's `crash_reports_url` is
  hard-coded `''` (`client-compat.ts:71`) and `POST /v1/crash-reports` is "planned" (SPECS.md:289-293). The
  `crash_reports_enabled` toggle has no view binding, so no one can turn it on.

### 2.3 Amp wire and keys (pp-cpp-amp, pp-cpp-crypto, pp-cpp-common)

| Format | Version and where | Checked first? | Other version today | Unknown field / type today | Range announced? | Ext. area | Alg. named | Frozen data |
|--------|-------------------|----------------|---------------------|----------------------------|------------------|-----------|------------|-------------|
| L1 packet | first nibble, wire 2 | yes | clean `VersionMismatch` via an unauthenticated VN reply that does not say who must update | unknown packet type dropped | VN lists versions, unauthenticated | none | no | header, VN, packet KAT |
| MSH handshake | hello byte `kMshVersion` 1 | yes | responder: Close with no reason, read by the dialer as "link table full", backoff 1–30 s and retry | trailing bytes fail the handshake; unknown message type: **stall** to the timeout | no | none | no (sizes imply ML-KEM-768 / ML-DSA-65) | transcript hash only |
| L3 frames, OPEN | rides the nibble | n/a | n/a | trailing bytes on OPEN/OpenAck/Close/Reset: silent drop, opener waits out its deadline; unknown frame type dropped (stalls a reliable channel if it used a seq); Reset code and Close reason ignored | n/a | none (`flags` unused) | n/a | DATA header |
| Carrier lanes | protocol id `/pp-amp/circuit-carrier/1.0.0` | n/a | `NoHandler` | unknown kind ignored | no | none | no | none |
| Addresses | `/adp/2/` component | yes | skipped silently, never dialled | exactly 8 components: anything appended is unparsable | one per address | none | n/a | strings |
| mDNS | TXT `wire=` | yes | ignored | unknown TXT keys ignored; with several `wire=` the last wins | single value | implicit | n/a | behaviour only |
| Channel-0 offer | leading byte 4 | yes | ignored, **and the reader never sends its own offer** | trailing bytes ignored (append-only) | one wire, one node version, empty features; sealed, not in the transcript | append-only tail | n/a | round trip |
| Keys, PeerId | none; raw fixed sizes; PeerId hashes key type 4 | n/a | wrong size refused | n/a | n/a | none | PeerId names its hash, not the key algorithm | PeerId vectors; **seeded keygen not frozen** |
| common archive | none (each format has its own) | n/a | n/a | trailing bytes refused | n/a | none | n/a | none |

Notes:

- **The refused-handshake path** is `MshAdpHandshake::Fail` → `ScheduleDropLink` → an ADP Close with an empty payload
  (`amp Connection.cpp:64-71`) → the dialer's sweep sees `PeerClosed` and calls `FailRefused`, "refused by the peer
  (its link table is full)" (`PeerLinkManager.cpp:1562-1576`). Today's readers ignore a Close payload, so a reason
  byte can be added compatibly.
- **The offer reader returns before replying** when the offer does not decode (`PeerLinkManager.cpp:1355-1368`), so
  neither side learns anything. `amp_wire` in the offer is never compared with the link's actual version; only
  pp-browser's N032 audit reads it (`pb ConversationsHub.cpp:170-182`). Apps send no offer data at all, only nodes.
- **Nothing in the handshake is downgradable today** because there is one version; a future dialer that fell back on
  a VN reply would be, since VN can be forged. The identity signature covers only the ephemeral KEM key, not the
  transcript; the Finished HMAC covers the transcript.
- **Seeded keygen matters:** pp-browser derives device, account and locker keys from seeds
  (`pb IdentityStore.cpp:85, 138-146`; `AccountLocker.cpp:62`; `LinkDeviceExchange.cpp` AccountFromSeed). A library change could
  silently re-key recovered identities, and no golden guards it in either repo.
- **Version-table rows DESIGN lacks:** the MSH handshake version 1, channel-0 session control (byte 2, exactly 6 B),
  the carrier protocol id, and the PeerId derivation.

### 2.4 Mesh protocols, media frames, DHT and directory (pp-cpp-mesh, pp-browser)

| Protocol / format | Version and where | Checked first? | Newer version today | Unknown op / field today | Id not served | How support is learned | Ext. area | Signed / alg. | Frozen data |
|-------------------|-------------------|----------------|---------------------|--------------------------|---------------|------------------------|-----------|---------------|-------------|
| All protocol ids | `/…/1.0.0` suffix, exact string | n/a | a different id is "not served" | n/a | clean `OpenAck` 1 "no handler", but only the media-relay client reads it | try-and-fail; `protocols[]` sent, never read | n/a | n/a | none |
| DHT `/pp-mesh/dht/1.0.0` | JSON `version` 1 | server yes, client never | server `bad_version`; client ProtocolError | `unsupported_op`; keys ignored | publish retries every 10 s, **no end** | try-and-fail | no | — | none |
| DHT record | `version` written, never read; `sign_version` 1 inside only | no | not checked | caps fixed in the sign bytes | — | — | no | ML-DSA-65; `signature_alg` named, **not signed** | round trips; contract text ≠ code |
| Directory `/pp-mesh/directory/1.0.0` and rows | JSON `version` 1; rows none | server yes, client never | server `bad_version`; client ProtocolError, then HTTP fallback | ignored; unsigned rows | refresh 300 s / 60 s | rows' `capabilities` | no | rows unsigned; keys name no algorithm | round trips |
| Dial-back, punch | `v` 1, never read | no | not checked | dial-back: text error, no code; punch server ignores an unknown op (opener stalls 8 s) | clean | try-and-fail | no | — | none |
| Circuit, media-relay control | `v` 1, never read | no | not checked | `bad_request`; after attach an unknown op is ignored without an ack; the quote's `rate` and `ceiling_amount` are `double`s with no unit (C6) | clean | caps + try | no | — | none |
| Media frames | high nibble of byte 0 = 4 | **yes**, before the seal | dropped **silently** at hop and receiver; frame version announced nowhere | optional extension kept; must-understand: receiver drops | n/a | not announced | **yes** (≤64 B TLV in the AAD) | AEAD | golden header + KAT |
| `/pp/rpc/chat` | inside the envelope | — | parse fails: no reply, silent close; `{"ok":true}` is sent before ingest and never parsed | — | read as `HandshakeFailed` | try | — | — | envelope's |
| `/pp/rpc/history`, `/pp/blob`, `/pp/account-card` | none; signed `PeerRequestAuth` v1 inside only | — | — | blob: **unknown op read as `fetch`**; account-card: unknown op answered `not_available`; all refusals silent | — | try | no | ML-DSA-65; no alg | none |
| `/pp/rpc/peer-announce` | tip `schema_version` 1 | yes | refused with text | "signed when present" breaks older verifiers on any new field | — | try | no | ML-DSA-65; no alg | none |
| `/pp/rpc/broadcast` | join ticket `schema_version` 1 | **no** | fails as "signature verify failed" | refusal text | — | try | no | ML-DSA-65; no alg | none |
| `/pp/realtime` control | `v` 1, never read | no | not checked | unknown `type` ignored | — | try | frames yes | AEAD | frames only |

Notes:

- **Media.** The hop drops a frame of another version without counting it (`mesh MediaRelayServer.cpp:409-412`), as
  does the 1:1 receiver (`pb CallMediaLegCoordinator.cpp:983-990`); group-call and broadcast receivers count failures
  but cannot tell a version from a key mismatch. A version mismatch therefore looks like "a call with no media". A
  frame's channel type other than `Forward` (0) or `LatestLossy` (1) is refused like any unknown type
  (COMPATIBILITY.md:111), so simulcast or screen share would need a tolerant reader in the floor first.
- **Protocol ids are matched as exact strings** (`amp ChannelMux.cpp:345-347`). `L4_PROTOCOL_KINDS.md` says ids are
  "kind-aligned (no dual advertise)", which the range rule replaces: a new wire version is a new id served beside the
  old one for the whole range. `/pp-mesh/media-relay-data/1.0.0` is not advertised.
- **`bad_version`** is not in `mesh docs/ERRORS.md`. `MESH_DHT.md:190-205` describes sign bytes the code does not
  write (`DhtRecordCodec.cpp:38-41`).
- pp-node's ledger protocols (`/pp-ledger/rpc/1.0.0`, `/pp-ledger/rpc-bulk/1.0.0`, pp-ledger `release/v1.8.1`) were
  not inventoried.

### 2.5 The relay's stored data

- **Tables carry no record versions.** The TTL attribute is `ttl`; point-in-time recovery is on `accounts` and `blobs`
  only (`relay docs/architecture.md:39-50, 85`). The S3 lifecycle rules and the real tables' TTL and backup settings
  live in Leo's infrastructure repo, not here, so they were not checked.
- **Rows written by older relays** are still read: the pre-cutover sender mapping (`records.ts:61-79`; it does nothing
  for legacy rows in stream query), legacy `llm_api_key`, the legacy sign window, unregister without `binding_id`
  (likely a 409 for rows without one, `devices.ddb.ts:59`, untested).
- **`putAccountCard` compares only `issued_at`** (`accounts.ddb.ts:203-206`): a v4 card not newer than a stored v3
  would come back `stale`, and the unserved v3 stays.

**What a revoke and an expiry leave**, one registration per Account ID (`registration.ts:298-308`):

| Data | On revoke | On expiry (90 days) |
|------|-----------|---------------------|
| Registration (AUTH, PROFILE, LOOKUP) | deleted in one transaction | TTL; the PROFILE item **stays** |
| Mailbox messages, dedup items | stay until their TTL (≤30 days) | same |
| Mailbox META item | **stays forever** (no `ttl`) | same |
| Large message bodies (S3) | deleted only by clear, eviction or a failed send; otherwise the bucket lifecycle (not in this repo) | same |
| Public icon and its `B#` record | **stay**, public at the CDN URL | same |
| Private files | `gcFiles` within about 7 days either way | same |
| Quota counter `Q#` | **stays forever** | same |
| Lockers | **stay** (no delete API; GET needs no registration, apparently so a device can restore) | same |
| Push devices | `purge` removes all of the account's; a lost purge is never retried | **stay forever** (no event; the check in `wakeRecipient` never runs for an old relay id) |
| Support conversation and crypto rows | **stay forever** (no `ttl`, no delete route) | same |
| Usage, limits | TTL (35 days; window end) | same |
| Diagnostics uploads | bucket expiry rule (not in this repo); hold no account id | same |

### 2.6 Local stores and node state

Exempt from the range ([DESIGN § Rollback](DESIGN.md#rollback)), but release 1 moves their floors. The shared JSON
check `SchemaVersion::Validate` (`pb SchemaVersion.cpp:32-42`) refuses a missing or newer version and accepts any
older one.

| Store | Version | Floor; below / above | Pre-release steps to drop at release 1 | Newer file refused, not overwritten? |
|-------|---------|----------------------|----------------------------------------|--------------------------------------|
| `profile.db` | `user_version` 8 | floor 4. Below: chats reset, public sessions kept. Above: recovery screen | 4→5, 5→6, 6→7, 7→8 | yes; **except `SqlitePostStore`** (no gate; tests only today) |
| `thread.db` | `user_version` 4 | floor 2. Below: that chat reset. Above: error for that thread | 2→3, 3→4 | yes |
| `object_index.db` (CAS) | **none** | — | — | n/a |
| `contacts.json` | 3 | floor 2. Below or above: **hard error, "delete the data directory"** | 2→3 | yes |
| `identity.enc` | 4 | floor 3. Below or missing: **unlock error, "wipe"** | 3→4 (shared with pp-node) | yes |
| `own_card.enc` | 1 | above: load error, then **overwritten** (`IdentityStore.cpp:862-865`) | — | **no** |
| `our_relays.json` | 2 | other: reseeded; above: saves refused | — | yes |
| `relay_pins.json` (app, pp-node) | 2 | other: empty, pins learned again | — | yes |
| `initiation_billing.json` | 2 | no floor; above: load error **ignored, then saved over** (`ConversationsHub.cpp:1402`) | 1→2 and the flat-root reader | **no** |
| `payment_promises.json` | 2 written, **never read** | none; not in DATA_LAYOUT | — | **no** |
| `preferences.json` | 16 | above: bootstrap error | the <16 invite-policy reset | yes |
| `machine.json`, `config.json`, `profiles.json`, `manifest.json` | 1 | above: error | manifest backend adoption (writes without the version check for a non-active profile) | mostly |
| `secrets.enc`, `vault.bin`, Keychain blobs, FileCipher envelopes | version byte 1, exact | refused | — | yes |
| `account_locker.json`, `relay_inbox_cursor.json`, `attachment_suppressions.json` | **none** | — | — | rewritten from memory |
| `client_compat.json` (cache) | **none**; document inside is 1 | refetched | — | a cache, but **drops fields it does not know** (`ClientCompat.cpp:90-115`) |
| Mesh and brief caches | 1, exact | ignored and rewritten | — | n/a |

pp-node uses the same code for `config.json`, `profiles.json`, `manifest.json`, `vault.bin`, `identity.enc` and
`relay_pins.json`; it opens no chat store, contacts or preferences. Its ledger directory was not inventoried.

## 3. Rows from RC015 (Kenneth) and for Leo

**RC015's rows**, each covered above:

- **Control lane:** § 2.1. No per-type versions; unknown types ignored; the risk is a new *required* step (a call
  step) failing silently on an older peer.
- **Group messages and bundles:** § 2.1. Signed statements and changes carry no outside version; unknown kinds, roles
  and presets stall or loop.
- **Attachment metadata:** § 2.1. Exact-end tail; the file ciphertext has no version or suite byte.
- **Account backup (the locker):** § 2.1. Dropped-on-write unknown keys, unversioned state.
- **Support client contract:** found only through `client-compat`'s `support` block (`pb ClientCompat.cpp:197-218`;
  no compiled id, no grant exemption on the app side). The support-worker on the relay speaks the app ↔ app
  formats, so it falls under that pair's range and every envelope, card or payload bump must reach it first.
- **AI client contract:** § 2.2. The app classifies LLM limits by English message text, relies on the `brf_llm_`
  key prefix, and never uses the relay's `llm_token`; SSE is tolerant.
- **Preferences:** `preferences.json` 16, a local store (§ 2.6).
- **The update-needed text:** the alert today, with `doc.message` overriding the translation (U12).

Their answers are in [RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows): part 1 decided, part 2 theirs with defaults.

**For Leo** (DESIGN § Decisions taken, items 3 and 8, and his infrastructure): A1 (the relay's release version and
ranges), A3 and C3 (prod's `client-compat` values), C2 (the legacy removals' timing), O1's dashboards, O3 (deploy
needs `integration`), O4 (identity rotation), O5's retention per table and the S3 lifecycle and TTL settings that live
in his repo, O6 (the crash-report destination), X11 (deploy per tag with Lambda versions and aliases).

## 4. Release-1 work list

**Need:** *needed* means the rule cannot hold without it; *worth* means cheapest now, before anything older exists
(each could also come later through the range rule). **Size:** a rough S / M / L estimate per item.
**Owner:** "code" unless a person is named for a value, text or infrastructure.

### T: Readers tolerate what they do not know

| Id | Need | Size | Repo | Item | Main files |
|----|------|------|------|------|------------|
| T1 | needed | S | pb | An unknown content type is stored as raw and shown as "update to view", its seq consumed (sent copies too); a pipeline test on SQLite | `ChatPayloadCodec.cpp:330-331`, `TranscriptBodyCodec.cpp:117-128`, `RelayReceivePipeline.cpp:1619, 2081` |
| T2 | needed | M | pb | A higher `envelope_version`: parse the routing and numbering fields loosely, close those seqs as empty, show a placeholder, stop the repair loop. Crypto stays strict | `MessagingJson.cpp:374-389, 861-876`, `HttpRelayClient.cpp:246-252`, `MeshDeliveryOrchestrator.cpp:1001-1005, 1501-1524`, `ChatSyncLogic.cpp` |
| T3 | needed | M | pb | After a good decrypt, a higher `payload_version`, unknown `sub_version`, higher `group_copy_version` or blob byte is "update needed" with the seq consumed, not DecryptFailed | `ChatPayloadCodec.cpp:350, 385`, `GroupE2ePayloadCodec.cpp:92`, `EncryptedPayload.cpp:27`, `RelayReceivePipeline.cpp:1801-1805, 2196` |
| T4 | needed | M | pb | ChatPayload and group-copy tails accept appended fields: an extension block with a must-understand bit, or trailing bytes ignored for a known `sub_version` | `ChatPayloadCodec.cpp` tails, `GroupE2ePayloadCodec.cpp:103`, `WIRE_SCHEMAS.md` |
| T5 | needed | S | pb | Every refusal after a good verify consumes the seq: annotation cap, membership apply, control apply, `psk_rotate` | `RelayReceivePipeline.cpp:1884-1889, 2037-2054` |
| T6 | needed | M | pb | Groups: an unknown change kind, role or preset neither stalls the DM nor loops catch-up; raw role and preset strings in the sign bytes; unknown changes counted in the hash or excluded on both sides | `GroupTypes.cpp:24-33`, `GroupRosterStatement.cpp:50-58`, `GroupChange.cpp:256, 359`, `GroupRosterSync.cpp:789-794`, `GroupRosterSyncCodec.cpp:86-97` |
| T7 | needed | M | pb | Locker: writers keep unknown keys and row fields; a state schema version and a record format version checked first; an older device says "update" | `AccountLocker.cpp:131-246`, `AccountStateCodec.cpp`, `AccountLockerWorkflow.cpp:315-318, 467-472`, `ACCOUNT_LOCKER.md` |
| T8 | needed | M | relay | Store the account card verbatim (signed bytes plus unknown keys), not normalized | `account-card.ts:244-250` |
| T9 | needed | S | amp | L3 readers ignore trailing bytes on OPEN/OpenAck/Close/Reset; an unknown frame type gets a coded RESET and new frame types use no channel seq; the Reset code reaches the terminal reason; an unknown MSH message type fails the handshake at once | `ChannelWire.cpp:202-268`, `ChannelMux.cpp:420-480`, `PeerLink.cpp:196-271, 341-346`, `MshAdpHandshake.cpp:294-296, 374-375` |
| T10 | needed | S | mesh | The punch server answers an unknown or wrong-phase op with a coded `result` | `PunchServer.cpp:356-409` |
| T11 | needed | S | mesh | DHT publish backs off when every answer was a refusal or "not served" | `DhtClient.cpp:292-294, 343-345` |
| T12 | needed | S | pb | App RPC servers answer `{"ok":false,"code":…}` instead of closing silently (chat, history, blob); the chat client parses the ack | `AmpDirectChatTransport.cpp:77-96, 298-305`, `AmpChatHistoryTransport.cpp:83-106`, `AmpChatBlobTransport.cpp:182-191` |
| T13 | needed | S | pb | Unknown ops refused, not misread: blob `op` (read as `fetch` today); account-card answers `unsupported_op`, not `not_available` | `MessagingJson.cpp:913-921`, `AccountCardRequest.cpp:27-28`, `AmpAccountCardTransport.cpp:61-73` |
| T14 | needed | S | pb | A permanent refusal backs the poll off (`426`, `400 unsupported_sign_version`, `wrong_audience` at the same PeerId), with one persistent update notice instead of an alert re-raised every 2 s | `RelayInboxCoordinator.cpp:114-123`, `BackgroundSyncScheduler.cpp:73-96`, `ClientCompatController.cpp:97-157` |
| T15 | needed | S | pb | The release-1 floor readers accept any `schema_version` ≥ 1 for the fields they know (`client-compat`, and `relay-identity`'s `== 1`); a timer re-check; decide prerelease and malformed-min handling | `ClientCompat.cpp:16-63, 155-170`, `RelayIdentity.cpp:29-31`, `client_compat_test.cpp:67` |
| T16 | needed | S | mesh, pb | Media readers forward or skip unknown channel types instead of refusing them, before simulcast or screen share need it | `MediaFrame.cpp`, COMPATIBILITY.md:111 |
| T17 | worth | S | pb | Retries slow to daily after a permanent refusal: other-relay registration, pp-node registration and heartbeat | `RelayRegistrationsWorkflow.cpp:137-140`, `node/main.cpp:496-549`, `NodeHeartbeat.cpp:207-226` |
| T18 | needed | S | pb | Billing and promise readers refuse or ignore an amount whose `currency` is not `pp_credit` (today any string is taken, and a missing one defaults to it), and range-check received amounts (≥ 0; promises check it only when created, `PaymentPromiseLifecycle.cpp:46`); RC017 item 2 | `InitiationBillingCodec.cpp:66, 89, 111`, `PaymentPromiseCodec.cpp:72`, `InitiationBillingStore.cpp:58`, `projects/pricing/DECISIONS.md` |

### V: A version every receiver checks first

| Id | Need | Size | Repo | Item | Main files |
|----|------|------|------|------|------------|
| V1 | needed | M | pb, relay | A version readable outside the signature, checked before verify, for the grant, roster statement / change / transfer / dissolved, the locker record, the KEM binding and `PeerRequestAuth` (history, blob, account-card); the relay side for grant, locker and binding. Designed in [SIGNED_FORMATS.md](SIGNED_FORMATS.md) | `RelayGrantSigner.cpp`, `MessagingJson.cpp:552-575`, `GroupRosterStatement.cpp`, `GroupChange.cpp`, `AccountLocker.cpp`, `KemKeyBinding.h`, `PeerRequestAuth.cpp` |
| V2 | needed | S | pb | The broadcast join ticket compares `schema_version` before verifying | `BroadcastJoinTicket.cpp:88-89, 170-206` |
| V3 | needed | S | mesh | The DHT record checks `version` before verifying; a newer record or unknown `signature_alg` is skipped as "update needed" and not counted as misbehaviour; STORE answers a typed `bad_version` | `DhtRecordCodec.cpp:83-120, 141`, `DhtClient.cpp:617-640`, `DhtServer.cpp:160-200` |
| V4 | needed | M | mesh | Circuit, media-relay control, dial-back and punch read `v` first and answer a newer one with a coded refusal; dial-back gains a `code` | `CircuitRelayServer.cpp:796`, `MediaRelayServer.cpp:811`, `DialBackServer.cpp:218-243`, `PunchLogic.cpp`, the `*Types.h` / `*Err.h` |

### U: A clean "update needed"

| Id | Need | Size | Repo | Item | Main files |
|----|------|------|------|------|------------|
| U1 | needed | M | pb | One "update needed" signal: `ReceiveFailure::NeedsUpdate` apart from DecryptFailed and BadSignature, a per-peer "needs a newer app" mark, the placeholder row, one rate-limited notice naming who must update. Text: Kenneth | `DeliveryNotice.{h,cpp}`, `RelayReceivePipeline.cpp:129`, `MeshDeliveryOrchestrator.cpp:904-930`, `TranscriptRml.cpp:876`, locales |
| U2 | needed | S | pb | A card with `card_version > 4` marks the peer "needs update", and the card-ask gate stops asking for a version it cannot read | `RelayReceivePipeline.cpp:1025-1046`, `AccountCard.cpp:210` |
| U3 | needed | S | pb | Text formats with a higher N read as "made by a newer app": locator, join link, PSK bundle, locker file (the link offer and reply do from C9: a higher version byte is "update needed"); the v2 URI scheme registered | `ContactLocator.cpp:66-70`, `GroupInviteLink.cpp:65-69`, `PskBundleCodec.cpp:69`, `Info.plist`, `AndroidManifest.xml` |
| U4 | needed | M | amp | A reason byte in the ADP Close (compatible: readers ignore the payload today): table full, admission, handshake failed, unsupported MSH version with ours, shutdown; the dialer maps each; an unknown ServerHello version is `VersionMismatch` | `Connection.cpp:64-71`, `PeerLinkManager.cpp:353-361, 1562-1576`, `PeerLink.cpp:490-494, 605-620`, `MshAdpHandshake.cpp:213-215, 307-310` |
| U5 | needed | S | amp, mesh, pb | `VersionMismatch` says who must update, carried to the reach record and the UI | `PeerLink.cpp:613-620`, `PeerReachRecord.cpp:37-38`, `CallConnectDiag.cpp:152` |
| U6 | needed | S | amp | The offer reader replies with its own offer even when the peer's does not decode, and records "unreadable offer, byte N"; leading byte 4 frozen, new fields append | `PeerLinkManager.cpp:1350-1370`, `Capability.h:26-30` |
| U7 | needed | M | mesh | DHT and directory: a version mismatch is a coded refusal naming the server's versions; clients map `bad_version` to VersionMismatch, not ProtocolError; codes added to `ERRORS.md` | `DhtServer.cpp:105-113`, `DhtClient.cpp:601-604`, `DirectoryServer.cpp:99-105`, `DirectoryClient.cpp:216-220` |
| U8 | needed | M | mesh, pb | Media frames do not fail silently: decode errors coded, drops counted by reason, the frame version announced (offer `features` or the call hello), a call whose frames fail on version shows "peer needs update" | `MediaFrame.cpp:96-113`, `MediaRelayServer.cpp:409-412`, `MediaRelayClientCoordinator.cpp:358-364`, `CallMediaLegCoordinator.cpp:983-990` |
| U9 | needed | M | amp, mesh, pb | "Protocol not served" surfaced as a code to every Amp transport, not as ChannelFailed / HandshakeFailed | `ChannelMux.cpp:390-400`, `PeerLinkManager.h`, `MeshPorts.h`, `AmpDirectChatTransport.cpp:290-292`, `DeliveryNotice.cpp:51-53` |
| U10 | needed | M | relay, pb | The relay names who updates: a distinct code `relay_upgrade_required` ([RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows)) for a newer card or `sign_version`; diagnostics the same | `relay account-card.ts:197`, `parse.ts:74`, `diagnostics.ts:76` |
| U11 | needed | S | pb | The app types "the relay is older" (`unsupported_sign_version`, U10's code, the card codes) and says "relay needs an update" apart from "update the app" | `RelayAudience.cpp:61-83`, `AppError.cpp:76-81`, `OurRelaysStore.cpp:242-263`, `MyRelays.cpp:22-24` |
| U12 | worth | S | pb | The update text is translated: per-locale messages, or the app's own text, not one server string. Kenneth | `ClientCompatController.cpp:141, 152` |

### A: Ranges announced; a node floor

| Id | Need | Size | Repo | Item | Main files |
|----|------|------|------|------|------------|
| A1 | needed | S | relay | The relay reports its release version and supported ranges (app, node) in `client-compat`, at `schema_version` 1, as `relay_version`, `app_range` and `node_range` ([RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows)) | `client-compat.ts`, `config.ts:43`, openapi, SPECS.md:77 |
| A2 | needed | S | relay, pb | A node floor (`min_node_version`, `10.0.0` at release, [RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows)) in `client-compat`; pp-node gains a `client-compat` reader (it has none) and refuses cleanly; the relay may `426` a node below it | `client-compat.ts`, `fields.ts:181`, pp-node |
| A3 | needed | M | amp, pb | The capability offer announces the range, appended under byte 4: wire versions and MSH versions spoken, release version and floor; after it arrives, check the link runs the highest common version, and never fall back on an unauthenticated VN; decide whether apps announce too; fill `features` | `Capability.h:14-30`, `Capability.cpp:45-111`, `PeerLinkManager.cpp:110-135, 1350-1380`, `pb ProductMesh.cpp:183-188`, `NodeMeshServices.cpp:186-188` |
| A4 | needed | S | pb | The `client_compat.json` cache keeps every release-1 field (switches, relay version and ranges, node floor) and gets a file version | `ClientCompat.cpp:90-115, 248-297` |

### C: A clean start

| Id | Need | Size | Repo | Item | Main files |
|----|------|------|------|------|------------|
| C1 | needed | M | pb | Leave the relay's legacy routes **before C2**: `/v1/search` → users/by-account, `blobs/*` → `cdn/*`, `files/*` → `blob/*`, `keys/rotate` and `llm_api_key` → `llm_token` and `token/renew`; the relay's LLM error codes instead of message text; "registered or guest" from the key's slot, not the `brf_llm_` prefix; drop `RelayFileUrl`'s hard-coded path ([RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) item 2) | `OrgBackendClientsImpl.cpp:196-224, 594-597`, `HttpBlobClient.cpp`, `RelayFileUrl.cpp:8-32`, `ConversationsHub.cpp:101-110, 3865-3937`, `AgentSessions.cpp:17-27` |
| C2 | needed | S–M | relay | Remove the legacy sign window (`94c6865`), the `blobs/*` / `files/*` / `GET /search` aliases, `llm_api_key` and `keys/rotate`, the pre-cutover sender mapping, in relay 1.0, deployed with app 1.1 (after C1; [RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) item 5) | § 2.2 legacy list |
| C3 | needed | S | relay config | `min_client_version` raised to app 1.1.0 ([RC024](DECISIONS.md#rc024--app-release-1-is-version-110)); prod's `upgrade_url` and `upgrade_urls` set (prod served `https://example.test/releases` on 2026-11-17). Leo | prod `CLIENT_COMPAT_JSON` |
| C4 | needed | M | pb | Local floors moved to release 1 and every pre-release step dropped ([RC017](DECISIONS.md#rc017--p0s-decisions-a-clean-local-start-the-currency-id-fixed-two-features-after-release-1)): `profile.db` 4→8, `thread.db` 2→4, `identity.enc` 3→4, `contacts.json` 2→3, `initiation_billing` floor 2 and its flat-root reader, the preferences <16 reset, manifest backend adoption | `SqliteThreadSchema.h:19-22`, `SqliteChatDbLadders.cpp:54-282`, `InitiationBillingStore.cpp:37-53`, `ConfigJson.cpp:939-956`, `ProfileRegistry.cpp:211-241` |
| C5 | decided | — | — | `identity.enc` and `contacts.json` lose the account below their floor, not just chats: every tester deletes the app and installs release 1, so their floors move too and no reset path is added (RC017 item 1, folded into C4) | — |
| C6 | needed (cheap now) | S | mesh, pb | The media relay quote's `rate` and `ceiling_amount` become integer credits with the unit they are per named in the quote (e.g. `rate_per`); today they are JSON `double`s with no unit. Always 0 today (paid quotes are refused until rails exist, pricing P004), so nothing depends on the shape; after release 1 it would be a new media-relay protocol version (RC017 item 2) | `mesh MediaRelayTypes.h:104-106`, `MediaRelayBundleLogic.cpp:101-102`, `MediaRelayServer.cpp:679`, `MediaRelayClientCoordinator.cpp:48`, `pb PricingTypes.h:19-28` |
| C7 | needed (before the freeze) | M | pb, relay | The account card, the locator's `relay` entry and relay links name the relay's operator key in place of `peer_id` (card v5, a pre-release cutover); builds carry the org relay's operator key per environment in place of its PeerId (`check_relay_peer_ids.sh` likewise); pp-node's `registration.peer_id` the same; amends MR008 ([RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) item 7). Designed in [SIGNED_FORMATS.md](SIGNED_FORMATS.md) | `AccountCard.cpp`, `ContactLocator.cpp`, `RelayPins*`, `DeploymentProfile.cpp`, `relay account-card.ts`, `ACCOUNT_CARD.md`, `SERVICE_ENDPOINTS.md` |
| C8 | built (P1–P5, #752–#756) | M | pb | The profile layout of [profile-model](../profile-model/DESIGN.md#on-disk): `manifest.json` 2 (network, no backend), `profiles.json` 2 (`active` per backend), opaque profile ids with `backends/{backend id}/` holding `registration.enc` (the registration slot and Brief LLM token, out of `identity.enc`), `client_compat.json` and `account_locker.json`; the profile's `services.json` (its promoted MCP, out of `config.json`); `machine.json` `show_tester_options`. A profile of the old layout is removed by the app with its keychain items and converted in place by pp-node (its identity kept) | `ProfileRegistry.cpp`, `ProfileManifest.cpp`, `IdentityStore.cpp`, `ProfileServices.cpp`, `Bootstrap.cpp`, `NodeBootstrap.cpp` |
| C9 | built (#766) | M | pb | Linking a device by a sealed QR handshake ([multi-device M027](../multi-device-account/DECISIONS.md#m027--a-device-links-by-a-sealed-qr-handshake-the-seed-rides-sealed-each-device-its-own-dek); [LINK_DEVICE.md](../../docs/contracts/LINK_DEVICE.md)): the new device's link offer v1 (a one-time ML-KEM-768 key, typed, with an extension area) and the main device's reply v1 (KEM ciphertext and an AEAD-sealed bundle: the recovery seed, intent join, expiry, optional preferred relay), each one binary QR code or the same as text, a 4-digit check before sealing; each device its own DEK. A clean break: the plaintext `pp-browser-link-device-v1` is gone, not read (testers reinstall, RC017). Its vectors: `link_handshake_vectors.json` | `LinkHandshake.cpp`, `LinkDeviceExchange.cpp`, `LinkOffer.cpp`, `ConversationsHub.cpp`, `SettingsSecurityActions.cpp`, `PinGateController.cpp`, `QrSvg.cpp` |

### O: Operating a live release (RC011)

| Id | Need | Size | Repo | Item | Main files |
|----|------|------|------|------|------------|
| O1 | needed | M | pb, relay | Versions counted: app and pp-node send `X-PP-Client: <product>/<version> <os>` on every relay call (HTTP, SSE, LLM, heartbeat); the relay counts per version per day, aggregates only; where it is counted is Leo's ([RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows)) | `HttpClient.cpp:196-203, 342-347, 503-508`, `SseClient.cpp:128-144`, `LlmClient.cpp:335-350`, `node/main.cpp:271-275`, `relay packages/shared/src/router.ts` |
| O2 | needed | M | relay, pb | Remote switches: `switches: {name: true \| false}` in `client-compat` and the app's reader at start-up (with A4) | `client-compat.ts`, `ClientCompat.cpp:150` |
| O3 | needed | M | pb, relay | The app ↔ relay behaviour suite against the local stack in both CIs on PRs; the relay's `deploy` needs `integration`; the lab nightly's variables set. Leo for the relay's CI | `relay ci.yml:72-76`, `pb lab-nightly.yml`, `pp_app_lab.py` |
| O4 | needed | L | relay, pb | Relay operator and keys apart, as in pp-ledger ([RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) item 7): the relay certificate (operator set with a threshold, offline, signs the relay's PeerId and signing key with a rising sequence); the relay serves it; the app and pp-node verify it, keep the highest seen, and take the audience PeerId from it; the signing key apart from transport; the operator-set ceremony and escrow. Custody and expiry: Leo. Designed in [SIGNED_FORMATS.md](SIGNED_FORMATS.md) | `relay relay-identity.ts`, `pb RelayPins*`, `RelayIdentity.cpp`, `DeploymentProfile.cpp:16-22, 56-63` |
| O5 | needed | M–L | relay, pb | Account deletion finished as two actions ([RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) items 1 and 8): *Delete account…* (revoke everywhere, delete the relay data in § 2.5 including lockers, wipe) and *Remove this device…* (local wipe, no revoke; the main device also edits the card). Revoke deletes the mailbox, icon, quota counter, push rows (a lost purge retried) and lockers; expired registrations cleaned on a schedule; Support kept for a stated period. Texts: Kenneth; periods and S3 rules: Leo | `pb Application.cpp:529-596`, `en.json:555-560`, `relay registration.ts:373-412`, `devices.ts:78-81`, support-store |
| O6 | needed | M | relay, pb | A crash-report destination: the planned `POST /v1/crash-reports` and `crash_reports_url`; the app's crash toggle bound in a view. Leo Client half answered (Kenneth, 2026-11-20): the opt-in is the existing "Collect crash logs" switch, off by default, and crash records leave the device only through the user's own "Send logs" action; the relay endpoint stays the backend owner's item, not decided by the client side. | `relay SPECS.md:289-293`, `client-compat.ts:71`, `pb SettingsController.cpp:880, 2542` |

### F: Frozen data ([DESIGN § Tests](DESIGN.md#tests))

| Id | Need | Size | Repo | Item |
|----|------|------|------|------|
| F1 | needed | M | pb | ChatPayload vectors B and C and each content type's bytes; a frozen envelope JSON parse; a signed roster statement; locker state JSON and a sealed snapshot from a fixed seed; locator, join link, PSK bundle (the link offer and reply: `link_handshake_vectors.json`, C9); control-detail samples; `PeerRequestAuth` sign bytes; peer-announce tip; join ticket; call-media control; a grant in `relay_sign_vectors.json` |
| F2 | needed | M | amp | Seeded MSH hello / payload / finished bytes and a transcript giving fixed session keys; OPEN, OpenAck, Close, Reset, FRAG; offer v4; session control; carrier and lane payloads |
| F3 | needed | S | crypto | Seeded ML-DSA-65 and ML-KEM-768 keygen (fixed seed, fixed key hash), verify of a frozen signature, decapsulation of a frozen ciphertext: they guard identity recovery |
| F4 | needed | M | mesh | DHT sign bytes and record JSON; directory request, response and row; circuit, media-relay, dial-back and punch v1 messages |
| F5 | worth | S | pb, relay | The shared vectors gain a newer-`sign_version` row (400), an unknown body key (ignored), and response fixtures with unknown keys for register/finish, poll, by-account, `client-compat` and `relay-identity` |

### S: Local stores

| Id | Need | Size | Repo | Item | Main files |
|----|------|------|------|------|------------|
| S1 | needed | S | pb | `own_card.enc` refuses a newer file and never overwrites it | `IdentityStore.cpp:853-883` |
| S2 | needed | S | pb | `initiation_billing.json` and `payment_promises.json`: a newer-file flag, no saves after a failed load; promises read and check their version and enter DATA_LAYOUT | `InitiationBillingStore.cpp:21-169`, `PaymentPromiseStore.cpp:42-107`, `ConversationsHub.cpp:1402-1404` |
| S3 | worth | S–M | pb | Versions for the unversioned stores: `object_index.db` (`user_version` and a ladder), `account_locker.json`, `relay_inbox_cursor.json`, `attachment_suppressions.json` | as named |
| S4 | before the posts client | S | pb | `SqlitePostStore` passes `chat_store_gate` before posts are wired | `SqlitePostStore.cpp:122-138` |

### X: Worth doing now

| Id | Need | Size | Repo | Item |
|----|------|------|------|------|
| X1 | worth | L | pb, relay, mesh | Signed extension areas (covered by the signature, unknown entries ignored, a must-understand flag) in the envelope, card, grant, roster statement and change, every relay sign domain, the DHT record, the peer-announce tip and the join ticket. Changes sign bytes, so cheapest as one pre-release cutover before release 1. Designed in [SIGNED_FORMATS.md](SIGNED_FORMATS.md) |
| X2 | worth | M | pb, relay, amp, mesh, crypto | Algorithms named: envelope `signature`, `key_init`, payload blob; card keys; relay command bodies (today only registration); an MSH suite id and the identity key in its libp2p encoding; directory row keys; `signature_alg` inside the DHT record's signed bytes. Designed in [SIGNED_FORMATS.md](SIGNED_FORMATS.md) |
| X3 | worth | M | amp | MSH v2: a TLV extension area in hello and payload, inside the transcript, carrying A3's ranges so the announcement cannot be stripped. A lockstep change, cheapest at release 1 |
| X4 | worth | S | pb | A suite / version byte in the attachment ciphertext, with room for chunking |
| X5 | worth | S | amp, mesh, common | Room in the other formats: session control ignores trailing bytes; addresses ignore components after `/p2p/<id>` and report a higher wire as "newer"; mDNS accepts several `wire=`; a shared extension helper and archive goldens in pp-cpp-common |
| X6 | worth | S | mesh, pb | The protocol-id rule for the range: a new wire version is a new id served beside the old one; advertise `/pp-mesh/media-relay-data/1.0.0`; read the peer's `protocols[]` to skip ids it does not serve |
| X7 | worth | S | pb | The control lane answers "control not understood", or calls negotiate capabilities, so a new required step on an older peer says "update" |
| X8 | worth | S | pb | Cards forwarded as the JSON received, not re-serialized |
| X9 | worth | S | relay | The support-worker answers an unreadable or newer message with a "not readable, please update" reply |
| X10 | worth | S | relay, pb | A version field in wake payloads; Android syncs on an unknown `type` instead of ignoring it |
| X11 | worth | S | relay | Deploy per version tag with Lambda versions and aliases (today `update-function-code` per function from the branch, so no rollback artifact and functions mixed mid-deploy). Leo |
| X12 | worth | S | pb | Read what the relay already says: `account_card_status` `stale` vs `stored`, card codes at finish, the stored `version` in a locker 409 |

### D: Decisions and docs

| Id | Need | Item |
|----|------|------|
| D1 | decided: after release 1 | The synced-keychain recovery tier (multi-device-account): additive; versioned with frozen data from day 1 when built (RC017 item 3) |
| D2 | decided: after release 1 | N031 signed node records (p2p-mesh): additive, with heartbeats and rows kept for the node range and A2's floor closing them (RC017 item 4) |
| D3 | decided | `pp_credit` is the protocol id for good; the display name stays free; the minor unit is one credit, a major unit later is display only; a checking reader in T18 (RC017 item 2) |
| D4 | needed | Version-table rows DESIGN lacks: MSH handshake 1, session control byte 2, the carrier id, the PeerId derivation, every row of § 2 |
| D5 | needed | Doc drift: `WIRE_SCHEMAS.md:61` and `COMPATIBILITY.md:68, 84` (history keys), `COMPATIBILITY.md:42` (JSON floors), `MESSAGE_ENCRYPTION.md:105` (group copy 2), `MESH_DHT.md:190-205` (sign bytes), `mesh ERRORS.md` (`bad_version`), `DATA_LAYOUT.md:147` and its missing stores, `SERVICE_ENDPOINTS.md:451` (revoke body) and its retired diagnostics endpoint, chat-storage `PHASES.md:307` ("unknown types rejected on ingest", the opposite of the rule) |
| D6 | needed | Other projects' docs: tick multi-relay R5, e2e-message-crypto c3++ / c4 and chat-storage `PHASES.md:306-320`, which are done in code; refresh the multi-relay, group-chat and chat-storage CURRENT_STATEs |

## 5. Breaking work in flight

No repo had an open PR on 2026-11-19, and no library's develop branch holds a format change beyond its pin, so
nothing in flight has to be split. Planned or half-done work that would change a format:

| Project | Item | Format | Status | Recommendation |
|---------|------|--------|--------|----------------|
| multi-relay | signed rotation statement for the org relay's key | became the relay certificate and the operator key in cards (RC018 item 7) | planned, no code | **before the freeze** (O4, C7) |
| pp-relay-apis | `POST /v1/crash-reports` | new endpoint | planned | **before the freeze** (O6) |
| pp-relay-apis | legacy aliases and LLM legacy keys | removal of old readers | in place, dated | **before the freeze**, after C1 (C2) |
| p2p-av-calls, broadcast | `video_hi`, simulcast, screen share | new media channel or stream ids | planned | the tolerant reader **before the freeze** (T16); the features wait |
| pricing | the `pp_credit` name | currency id in billing rows and on the wire | decided (RC017) | id fixed now; display name free (D3, T18) |
| multi-device-account | seed in the synced keychain | a store other installs read | planned | after release 1, additive (D1) |
| p2p-mesh | N031 signed short-lived node records | new signed object | planned | after release 1, additive (D2) |
| social-posts | P2 client (`post-entry-v1`, already with a version byte and extension section) | post log, head and media | half-done, library only | wait; S4 first |
| media-hop-reachability | circuit protocol v2 (`bridge_path`, `sub_bridge`, `max_hops`) | circuit relay | planned | wait; ship as a new id beside v1 (X6) |
| pricing | P004 paid-hop handover | new hop payment controls | deferred | wait |
| group-chat | new `invite_policy` values; padding and send jitter | policy enum; copy sizes | planned | wait (T6 makes unknown presets safe) |
| call-path-resilience, media-client-layers | metered state; stream plan | additive fields or messages | planned | wait |
| multi-device-account | d4 (only the main device holds the account key), d6, person-level preferences in the locker | relay auth, directory row, locker state | planned | wait (T7 makes locker additions safe) |
| content-cas, p2p-mesh, peer-scoped-broadcast | catalog rpc, piece manifests, `message_relay`, B1 tree fields | new protocols and fields | planned or half-done | wait; additive under X6 |
| privacy, chat-storage | message requests, FTS DB and other local state | local only | planned | exempt; a new local DB gets a version from day 1 |

DESIGN's "Not yet designed" topics: two would change a format, additively. *Relays and nodes run by others* needs
the relay to announce its version (A1) and nodes a field saying an update is due (A2). *Libraries change the wire
too* is covered by T9–T11, V3–V4, U4–U9 and F2–F4.

## 6. How this was checked

Five read-only passes, one per area (app ↔ app; app and pp-node ↔ relay, in two halves; the Amp wire and keys; mesh
protocols and media; stores and planned work), each citing path:line. Before an item went into § 4, the code behind
the findings that drive it was read again directly, among them: the unknown-content-type persist failure, the
envelope Gap stall, the empty Close read as "table full", the MSH stall on an unknown message type, the unanswered
unreadable offer, `signature_alg` outside the DHT sign bytes, the blob op read as `fetch`, the join ticket's
uncompared `schema_version`, the DHT 10 s publish retry, the chat ack before ingest, `relay-identity`'s `== 1`, the
missing User-Agent, the `426` alert, `own_card.enc` overwritten, the ignored billing and promise load errors, and
`contacts.json`'s hard error below its floor.

Not inventoried: pp-ledger's protocols and pp-node's ledger directory; the relay's infrastructure repo (S3
lifecycle, TTL and backup settings); broadcast RPC ops and the media-key wrap in depth; what a sender does after a
missing Amp ack.
