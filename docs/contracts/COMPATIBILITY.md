# Compatibility policy

**Tier:** contract

**Status:** Normative policy (contracts tier).  
**Related:** [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md), [DATA_LAYOUT.md](DATA_LAYOUT.md), [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md), [AT_REST_ENCRYPTION.md](AT_REST_ENCRYPTION.md), [docs/README.md](../README.md).

This doc answers: what happens when the data directory has leftovers, or when a **newer peer / API** talks to this build? Goal: **no crash**; degrade or fail clearly; avoid dual parsers for obsolete wire (D016).

**Release-1 freeze (since 2026-11-19, until release 1 ships):** a breaking change to any format between separately
released components (wire, signed bytes, HTTP, the relay's stored data) lands only if it is an item of the release-1
work list ([release-compatibility INVENTORY § 4](../../projects/release-compatibility/INVENTORY.md#4-release-1-work-list));
the PR names the item. Anything else is additive or waits for the range rule that starts with release 1
([PHASES § P1](../../projects/release-compatibility/PHASES.md#p1--freeze)). Local stores stay exempt.

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
| `contacts.json` of a `schema_version` below 3, newer than this build's, or none | Contacts **error** (the store refuses the file); wipe the profile data ([DATA_LAYOUT § Schema versions](DATA_LAYOUT.md#schema-versions)) |
| `identity.enc` of a `schema_version` below 4, or none | Unlock **error**; wipe the profile |
| `initiation_billing.json` of a `schema_version` below 2, or none (the flat form before `peers`); `payment_promises.json` other than 2, or none | Refused: no rows are read, and the file is not written over (that run's rows live in memory) |
| **Newer** `own_card.enc`, `initiation_billing.json`, `payment_promises.json`, `account_locker.json`, `relay_inbox_cursor.json`, `attachment_suppressions.json`, `object_index.db` (stores read after start-up) | Refused, never written over: the card is read from `peer_cards`, billing, receipts and locker state live in memory that run, the inboxes fetch from their oldest message, deleted attachments and the content index refuse changes. The four unversioned ones (`account_locker.json`, `relay_inbox_cursor.json`, `attachment_suppressions.json`, `object_index.db`) say their version from now on; one without it is read as today's |
| A contact row without `local` (the flat form before the local/remote split) | Only its id is read |
| Config keys from before a rename (`libp2p`, `mcp`) | Ignored (read `mesh`, `promoted_mcp`); a config without `backend` is on the default backend whatever URLs it carries, and one without `llm.preset` is on "brief" |
| Orphan `threads/{id}/` with `thread.db` | Re-cataloged as AI threads |
| Corrupt / **newer** `manifest` / prefs / `profiles.json` / `config_version` | Bootstrap **error** → process exit 1 (not a crash) |
| Wrong `vault.bin` magic/version; bad `identity.enc` | Unlock **error** |
| SQLite `user_version` of `profile.db` / a `thread.db` ([DATA_LAYOUT § Schema versions](DATA_LAYOUT.md#schema-versions), D105) | `0`: created at the current schema. From the floor (profile.db 9, thread.db 4 — release 1's schemas, RC017) up: migrated step by step, a backup copy restored if a step fails (then refused). Below the floor: **reset** — profile.db: the whole chat store (`threads/`; the account, contacts and settings stay) but its public sessions, kept as Reset chats keeps them, a thread.db: that chat's messages — logged, and the app says "your chats were reset" once after start-up. Newer, or an upgrade rolled back: left shut, and the app offers the start-up recovery (update the app, or Reset chats…). All of it after unlock, never before |

**Policy:** unknown junk is safe. An official file of an older form than its store reads is refused or ignored like a newer one, except where the store's ladder upgrades it ([DATA_LAYOUT § Schema versions](DATA_LAYOUT.md#schema-versions)): the SQLite chat store migrates from its floor and resets below it; a JSON store's floor is its current version. Every floor is release 1's schema, with no step from before it ([release-compatibility RC017](../../projects/release-compatibility/DECISIONS.md#rc017--p0s-decisions-a-clean-local-start-the-currency-id-fixed-two-features-after-release-1): every tester reinstalls release 1). A disk migration is this device's own data moving forward — not a bridge to older peers ([§ No bridges](#no-bridges-pre-release)). Because phones install only the latest app, the latest app carries every step from the floor up, and the floor moves only at a milestone. Official filenames with unsupported **newer** schemas fail bootstrap (start-up stops with the error, process exit 1) until the app is updated or the profile/data dir is reset — except the chat store, which opens after unlock: newer than this build, or a migration that failed and was rolled back, it stays shut and the app shows the start-up recovery (update the app, or Reset chats…), whose reset goes on into the normal start-up. pp-node opens no chat store. See D016 (no legacy JSON/wire import), D069 and D105 (the ladder).

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
| Contact locator JSON (`pp-browser-contact-v1:`) | Ignore unknown keys; the optional `relay` (multi-relay R4, MR008: `{origin, peer_id}`) is read when its origin is within MR004's bounds and it names a PeerId, else dropped ([ACCOUNT_CARD § Locator](ACCOUNT_CARD.md#locator-qr-code-invite-link)) |
| Signature / AAD | Unknown envelope keys are **not** signed; AEAD/sign failures are **hard** |
| Account locker state JSON | Unknown keys and row fields are ignored. `device_names` (display names, [ACCOUNT_LOCKER § Device names](ACCOUNT_LOCKER.md#device-names)) was added as an optional key: an older build ignores it on read and drops it on write; devices that know it show "New device" until the next sync puts it back |

### Target behavior (forward-compat)

| Condition | Desired outcome |
|-----------|-----------------|
| `envelope_version` == known | Full ingest |
| `envelope_version` > known | **Done** (INVENTORY T2): its routing and numbering read loosely, nothing decrypted or verified; its seq closed as empty (so the thread leaves Gap and no repair asks for it again; a verified message at that seq is still taken), one "update to view" row with no seq, the sender marked (`NeedsUpdate`). On the relay poll, in history and over a link alike. It moves no epoch |
| Unknown `content_type`, or a higher ChatPayload `payload_version` / `sub_version`, `group_copy_version` or blob version byte, after the signature verified (and the AEAD opened, where it is inside) | **Done** (release-compatibility INVENTORY T1, T3): kept as "Update the app to view this message." — the ChatPayload bytes stored as received, so the updated app reads the row (a newer blob or group copy keeps nothing readable: a local stand-in row, [WIRE_SCHEMAS § ChatPayload](WIRE_SCHEMAS.md#binary-layout-payload_version--1)); its `sender_seq` consumed; sent copies from our own newer devices alike. One signal, `ReceiveFailure::NeedsUpdate`, apart from `DecryptFailed` and `BadSignature` (U1): the sender is marked "uses a newer app" in the chat's header, and the user gets one notice per sender a day naming who sent it and that this app needs the update; no failure line (the row is the line) |
| Any other refusal after the signature verified: a membership or control update that does not apply, the reaction cap, a `psk_rotate` refused | **Done** (T5): refused, and its `sender_seq` consumed — closed as empty, so sync never stalls and a resend is a late fill, not a gap or (in a private chat) a compromise. A `psk_rotate` with a `wrap_kind` this app does not know is `NeedsUpdate` |
| An account card of a higher `card_version` (riding an envelope) | **Done** (U2): not kept; its account marked as using a newer app, and not asked for its card for 24 h (`CardAskGate`) |
| Decrypt / verify / AEAD failure | Hard reject that message (trust boundary) |
| Legacy shapes (`thread_id`, `body.text`, `content_b64`, `public_relay`) | Hard reject (D016 / D090) — not “newer”, obsolete |

**Implementation note:** the rows marked **Done** are the receiver's behaviour since release-compatibility P3 (2026-11-20); the update-needed words and display follow [RC018](../../projects/release-compatibility/DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) part 2's default until Kenneth decides them. Hard rejects stay for crypto, older versions and known-legacy shapes.

### Clean cutovers (pre-release)

Taken whole, with no old/new bridge: every app updates.

| Change | Old ↔ new |
|--------|-----------|
| Account card v2 — the device list with each device's keys ([ACCOUNT_CARD.md](ACCOUNT_CARD.md), M021 d1) | Cards between apps before and after it are refused (card exchange over Amp, contact cards, roster cards): a contact added across the line has keys only once both update. |
| Envelope v2 / sign v3 — one record per account with a copy per device, device signatures, sessions per device pair ([WIRE_SCHEMAS.md](WIRE_SCHEMAS.md), M021 d2 / M022) | Messages, group copies and call signaling between apps before and after it are refused (unknown `envelope_version`). Sessions with a peer's devices start fresh; account-level sessions are kept. Needs the relay that accepts an account's records to itself (messenger, M022) — deployed first. |
| Sign v4 — group copies name their group only inside their ciphertext, group stream ids are a hash of the group and the pair ([WIRE_SCHEMAS.md § Group](WIRE_SCHEMAS.md), group-chat G018 item 7) | Every envelope's signature changes (direct and control too), so messages, group copies and call signaling between apps before and after it are refused — as a signature that does not verify (`envelope_version` stayed 2: the receiver cannot tell an older sender from a forgery; a `control.unverified reason=signature_mismatch` line, [DIAGNOSTICS.md](DIAGNOSTICS.md)); an envelope whose route names a group is refused. Group history stored on the relay under the earlier stream ids is not fetched again. No relay change (messenger treats stream ids as opaque). |
| Amp wire v2 — sealed packets, an 8 B association id, a 1232 B datagram ([ADP.md](ADP.md), [A029](../../projects/adp/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H1; pp-cpp-amp v4.0.0) | No Amp link between an app or node before it and one after it: each refuses the other's packets (version byte), so chat, sync, calls and the mesh itself need both ends updated. Apps and pp-node ship it together. |
| One media frame, an 18 B header and a derived nonce ([WIRE_SCHEMAS § Media frames](WIRE_SCHEMAS.md#media-frames), [A029](../../projects/adp/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H2; pp-cpp-mesh v5.0.0) | No call or broadcast media between an app before it and one after it: each refuses the other's frames (version), and a hop before it drops them. Apps and pp-node ship it with amp wire v2. |
| Media frame v4 — a 22 B core with the capture time, an extension block ([WIRE_SCHEMAS § Media frames](WIRE_SCHEMAS.md#media-frames), [V066](../../projects/p2p-av-calls/DECISIONS.md); pp-cpp-mesh v8.0.0) | No call or broadcast media between an app or hop before it and one after it: each refuses the other's frames (version 3 against 4). Apps and pp-node ship it together. Later optional fields go in the extension block and need no cutover. |
| Addresses say their wire version, `/adp/2/` ([ADP.md § Addresses](ADP.md#addresses), [N032](../../projects/p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes) item 3; pp-cpp-amp v5.0.0) | An app or node on pp-cpp-amp before v5.0.0 cannot parse an address written after it, so it cannot dial it. An address of another wire version is skipped before dialing (the versionless `/adp/1.0.0/` too since the row below). Ships with amp wire v2. |
| Call audio in 40 ms Opus packets ([CALLS.md § Audio packets](../architecture/CALLS.md), [A029](../../projects/adp/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) step 1) | An app before it hears nothing from an app after it: it decodes every packet into a 20 ms buffer, which a 40 ms packet does not fit. An app after it plays both, so it hears an older peer. Calls, group calls and broadcasts alike. |
| Only versioned addresses parse: `/adp/1.0.0/` is refused ([ADP.md § Addresses](ADP.md#addresses), 2026-11-12, no bridges) | A contact's, a config's or a seed list's address stored in the versionless form is skipped like an unparsable one: re-add the contact or fix the config. An app or node before it still writes and dials `/adp/2/`, so the two reach each other at every current address. |
| LAN discovery says its wire version: mDNS TXT `wire=<N>` beside `peer=` ([ADP.md § Addresses](ADP.md#addresses), 2026-11-12) | An app after it does not hear one before it on the LAN (its announcement says no version, so it is not a browse result); one before it hears one after it as before. A same-LAN peer is still reached by every other path. |
| Channel 0's capability offer says what the node runs: leading byte `4`, its wire version, node version and features inside it, no separate extension ([AMP-CHANNEL § Channel 0](AMP-CHANNEL.md#channel-0--capability-plane), N032; 2026-11-12) | A link between an app or node before it and one after it comes up, but neither takes the other's offer (`1` and `4` are each refused by the other's decoder): no listen addresses or protocols are learned over the link, and the node's claims are not seen, so they cannot exclude it. |
| Nested links always speak the reliable lane, and send every handshake message in pieces of one outer frame ([AMP-CHANNEL § Carrier payloads](AMP-CHANNEL.md), 2026-11-12) | A nested link (through a relay) between an app or node before it and one after it still works: the newer side sends no lane probes and wraps reliable frames in `LaneData` from the start, and the older side takes the lane up on the first lane frame it gets (until then the newer side reads its plain sealed frames). A large handshake message now goes in pieces on its first send, which the older side already reads (its resends came so). |
| A channel OPEN carries its max message bytes (2026-11-12) | An OPEN without the trailing u32 is refused; every build writes it, so no current peer is affected. |
| Circuit and punch refusals by `code` only ([pp-cpp-mesh docs/ERRORS.md](https://github.com/people-post/pp-cpp-mesh/blob/main/docs/ERRORS.md), 2026-11-12) | A refusal with no code (a relay or introducer before codes) is a plain failure on an app after it: no retry of the not-reserved case, no busy / scope handling. Every current node sends codes. |
| The DHT: a third party's record without its `public_key_b64` counts against its server ([MESH_DHT.md](MESH_DHT.md), 2026-11-12) | A node before records carried the key serves them without it: a node after it drops them and lowers that server's soft reputation. Every current node stores and serves the key. |
| The M024 bridges dropped: the control lane goes per device whenever the sender holds the peer's card (single-device accounts too), and a call's media key goes only as `media_key_b64` inside the lane copy — `wrapped_key_b64` is neither sent nor read ([M024](../../projects/multi-device-account/DECISIONS.md#m024--the-control-lane-goes-per-device-a-calls-media-key-rides-the-copy), [WIRE_SCHEMAS § Call media keys](WIRE_SCHEMAS.md), [P2P_MESSAGING § The control lane](../architecture/P2P_MESSAGING.md#the-control-lane)) | An app before M024 refuses every lane record from an app after it whose sender holds its card (device copies): calls do not ring there, and card and copy asks go unanswered; it also finds no key it reads (`wrapped_key_b64`) in an invite or `call_media_key`. An app after it takes no key from an app before M024, which sends only `wrapped_key_b64`: the call gets no media key. A first contact (no card held) still goes as the account copy both ways. An app on the bridge release and one after it work together: it sends and reads both device copies and `media_key_b64`. |
| Calls: `call_sdp`, `call_ice` and the `call_join` alias are no longer known control types, and an invite without `video_allowed` does not allow video ([CALLS.md § Inbound call control](../architecture/CALLS.md)) | Nothing has sent `call_sdp` / `call_ice` since calls moved to Amp, and `call_join` was the accept's former name: an app after it ignores all three like any unknown control type, so a `call_join` from a very old peer does not answer the call. An invite from an app before V035 (`media_mode: "video"` without `video_allowed`) rings with video not allowed; every app since V035 sends `video_allowed`. |
| Call rows lose their kind: the legacy `Broadcast` session kind is gone ([media-client-layers l6](../../projects/media-client-layers/PHASES.md)) | An app after it neither reads nor writes `session_kind`. A leftover `profile.db` row written as a broadcast by a build before l6 would load as an ordinary call row, but leftover DBs are not a supported upgrade (§ On-disk). No wire change. |
| Call and broadcast media ride only the data lane, on every path: a hop behind a circuit is reached by a nested link with both lanes, and nothing carries media on a reliable channel ([V065](../../projects/p2p-av-calls/DECISIONS.md#v065--call-audio-keeps-a-delay-budget-audio-before-video), [MEDIA_CHANNELS § Channel type](MEDIA_CHANNELS.md#channel-type-relay-qos)) | A hop that serves no data lane (pp-cpp-mesh before v3.1.0) hosts no media after it: the attach fails and the app picks another hop. A hop after it drops media that arrives on a control channel (an app before it, which fell back there; `pp_media_relay_dropped_frames_total{reason="control_channel"}`). A relay after it refuses a circuit tunnel to a media-relay target, so an app before it cannot reach a hop behind a circuit through one. |
| A media frame's channel type is `Forward` (0, audio) or `LatestLossy` (1, video); `BestEffort` (2) is refused like any unknown type ([WIRE_SCHEMAS § Media frames](WIRE_SCHEMAS.md#media-frames)) | The values apps send are unchanged (audio 0, video 1), so apps and hops on either side still forward each other's frames. A frame stamped 2 — which no app sends — is refused by a hop after it. |
| Account IDs everywhere: no `relay:` ids, the relay addressed by Account ID ([multi-relay MR001](../../projects/multi-relay/DECISIONS.md#mr001--inboxes-are-keyed-by-account-id), [SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md); 2026-11-12) | Needs the relays of R1 (`www` / `messenger` with inboxes by Account ID, relay API sign version 2) — they ship together: an app before it cannot register, send or poll on such a relay, and one after it cannot on an older relay (every field and signature changed). Envelope v3 / sign v5 (no `sender_relay_id`), the contact card `sub_version` 3 and the newcomer without `relay_user_id`: messages, group copies and call signaling between apps before and after it are refused (unknown `envelope_version`), as are contact cards between them. A mesh directory row without `account_id` is refused (pp-cpp-mesh v9.0.0). Stored `relay:` ids were mapped or dropped by disk steps that release 1 no longer carries (RC017: every tester reinstalls); the relay inbox cursor starts over. |
| Relay grants: a send carries the recipient's grant, else it is a stranger's ([ACCOUNT_CARD § Relay grants](ACCOUNT_CARD.md#relay-grants), multi-relay MR005; 2026-11-12) | An app before it neither issues nor attaches grants (it predates the relay too, above). Between apps after it, the first run grants every contact and member; until a peer's grant arrives, sends to it go as a stranger's (no push, the stranger quota, or `payment_required` when it charges); the grant goes on the first run and again whenever the peer is heard from until it confirms. |
| Account card version 3 — the card signs the account's relays and its initiation floor; the relay takes the card at registration and reads the stranger floor from it, and registration sends no floor field ([ACCOUNT_CARD.md](ACCOUNT_CARD.md), [SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md), multi-relay MR002 / MR004 / MR005; 2026-11-13) | Cards between apps before and after it are refused both ways (card exchange over Amp, contact cards, roster cards, an envelope's sender card): until both update, a contact's devices are known only from cards kept before. Needs the relay that takes version 3 at `register/finish` — they ship together: an app after it cannot register on a relay before it (its card is refused, and with it the registration), and a relay after it refuses an older app's version 2 card. Kept cards stepped on disk (`profile.db` 6 → 7, a step release 1 no longer carries, RC017): ours was re-issued with its device list, anyone else's asked for again. |
| Relay identity and the audience — every signed relay command names the relay it is for (its PeerId) at a new sign version: registration v3, relay API v3, revoke / profile / icon / heartbeat / LLM token / locker / blobs / files v2; revoke names its `registration_id`, unregister its `binding_id` ([SERVICE_ENDPOINTS § Relay identity and the audience](SERVICE_ENDPOINTS.md#relay-identity-and-the-audience-multi-relay-mr007), [multi-relay MR007](../../projects/multi-relay/DECISIONS.md#mr007--relays-have-a-node-style-identity-every-signed-relay-command-names-it-as-its-audience); 2026-11-14) | A relay after it refuses every signed call of an app or pp-node before it (`401 wrong_audience`: no registration, mail through it, push or heartbeat; search, lookups and `client-compat` still work), and an app after it signs nothing for a relay before it (no `GET /v1/relay-identity`: nothing is pinned). The relay goes first (qa), then app, pp-node and prod together with `client-compat`'s `min_client_version` raised. |
| Account card version 4 — each relay on the card is `{origin, peer_id}` ([ACCOUNT_CARD § Account card](ACCOUNT_CARD.md#account-card), MR007; `profile.db` 7 → 8, a step release 1 no longer carries; 2026-11-14) | Version 3 cards are refused everywhere (registration `invalid_account_card`, directory answers, the card exchange, envelopes, group bundles, contact cards): a contact on the other side has keys only until its newer card arrives. A relay after it does not serve a stored version 3 card. |
| A relay address carries its PeerId — the app expects each relay's PeerId from its address (a relay link `https://host#<PeerId>`, the build's `PP_RELAY_PEER_ID_PROD` / `_QA`, the config's `relay.peer_id`, our own card, the locker) and only checks `GET /v1/relay-identity` against it; the QR locator's `relay` becomes `{origin, peer_id}`; `our_relays.json` 2 (each relay with its `peer_id`), `relay_pins.json` 2 (expected identities), the locker's `relays.list` of `{origin, peer_id}`, `contacts.json` `remote.locator_relay` `{origin, peer_id}` ([SERVICE_ENDPOINTS § Pins](SERVICE_ENDPOINTS.md#pins), [multi-relay MR008](../../projects/multi-relay/DECISIONS.md#mr008--a-relay-address-carries-its-peerid-no-first-use-trust); 2026-11-15; with MR007, never shipped) | A locator from an app before it carries an origin alone, which an app after it drops (the contact is still added; its first message waits for its card or goes to our preferred relay); an app before it ignores the object. An MR007 build's `our_relays.json` 1 and `relay_pins.json` 1 are not read: the list is seeded again from the deployment and the pins are expected again from the addresses (a relay added by hand there is added again by its link). No relay change: the identity endpoint and the audience are MR007's. |
| The relay is named by its **relay id** and proved by its **certificate** (release-compatibility [SIGNED_FORMATS § 3](../../projects/release-compatibility/SIGNED_FORMATS.md#3-the-relay-operator-set-and-certificate), RC020; the signed-format cutover, on `cutover/signed-formats`): relay links `https://host#relay:…`, the QR locator's and `contacts.json`'s `relay` `{origin, relay_id}`, `our_relays.json` 3, `relay_pins.json` 3 (relay ids and the highest certificate each), config `relay_id`, `PP_RELAY_ID_PROD` / `_QA` with `config/relay-certs/<env>.cert`, pp-node `registration.relay_id`; `GET /v1/relay-identity` v2 `{relay_id, certificate_b64}`, `client-compat`'s `relay_certificate_b64`; the audience is the certificate's transport PeerId ([SERVICE_ENDPOINTS § Relay identity](SERVICE_ENDPOINTS.md#relay-identity)) | A clean cutover: nothing older is read. A `#Qm…` link is refused, a locator or contact naming a `peer_id` relay reads as none, the files of the row above are seeded or expected again, and a relay answering `relay-identity` 1 is no identity (not used). The card (v5, W2) and the locker (W3) take relay ids in the same cutover. |
| Account card **version 5**, the key card and **KEM binding v2** (release-compatibility [SIGNED_FORMATS § 2.2–2.3](../../projects/release-compatibility/SIGNED_FORMATS.md#22-account-card-v5-and-key-card), RC020; the signed-format cutover, on `cutover/signed-formats`): typed keys and signature, relays `{origin, relay_id}`, an extension area (`ext_b64`), domains `pp-browser:account-card` / `pp-browser:kem-binding` without `-vN`; the key card's and directory row's `kem_binding_b64` (the binding blob) for `kem_binding_signature_b64`; register/finish sends `kem_binding_b64` ([ACCOUNT_CARD](ACCOUNT_CARD.md)) | A clean cutover: nothing older is read. A version 4 card, a v1 binding or an untyped key is refused (a directory row's is dropped); a newer card, binding or algorithm reads as *needs a newer app*, never as bad keys. Stored v4 cards no longer read and are asked for again; testers reinstall, and the relay's pre-release cards are deleted (Q12). |
| Media-relay refusals are read by their code only ([pp-cpp-mesh docs/ERRORS.md](https://github.com/people-post/pp-cpp-mesh/blob/main/docs/ERRORS.md), 2026-11-12) | A hop before coded refusals, which said why only in text, now reads as a plain failure (`Generic`) to an app after it: the app moves on to its next hop either way. |
| A signed relay body names its `sign_version`, and a relay answers an older one (or none), or an account card older than version 4, `426 client_upgrade_required` before anything else; the app shows its update screen for the org's relay and *Needs a newer version of the app* for another of its relays; client-compat gains `upgrade_urls` per platform ([SERVICE_ENDPOINTS § Client compatibility](SERVICE_ENDPOINTS.md#client-compatibility-discovery); 2026-11-15) | An app or pp-node before it sends no `sign_version`: a relay after it answers its every signed call 426 (an app before it shows that as a failure; after it, every later cutover of a sign version or card reads as "update the app"). A relay before it ignores the field, and an app after it reads no `upgrade_urls` from it (`upgrade_url` as before). The relay goes first with the app, as for the audience. |

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

**Ends at release 1** (accepted 2026-11-17, release 1 not shipped yet): from then on, every app, pp-node and relay works with every other one
whose version is inside the supported range — [release-compatibility RC001](../../projects/release-compatibility/DECISIONS.md#rc001--compatibility-within-a-supported-range-from-release-1-on).
Until release 1 ships, this section holds.

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
- **Client-compat discovery:** unauthenticated `GET /v1/client-compat` on the relay base URL ([SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md#client-compatibility-discovery)). Compare local app semver (`PP_BROWSER_RELEASE_VERSION`) to `min_client_version` / `latest_client_version`. Below min → blocking update dialog; below latest → a card at the top of the chat list (hidden by "Later", back once after 7 days, again at once for a still-newer version). The app's own translated line is always the text; the relay's `message`, when it sends one, is an extra smaller line under it on the card and a second paragraph in the dialog (plain text, capped, RC018 part 2). Fail open on network error; cache TTL 6h under the profile dir, checked again (forced) when the app comes to the foreground at most every 15 min, and when the org's relay answers `426 client_upgrade_required` (the blocking dialog whatever the floor). This is a **relay/service** floor only.
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
