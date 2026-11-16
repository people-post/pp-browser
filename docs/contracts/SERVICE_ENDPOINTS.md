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
  "relay": { "base_url": "https://backend.example.test/api/relay", "transport": "http", "peer_id": "Qm…" },
  "directory": {
    "base_url": "https://backend.example.test/api/relay",
    "transport": "http",
    "providers": [
      { "base_url": "https://backend.example.test/api/relay", "transport": "http" }
    ]
  },
  "registration": { "base_url": "https://backend.example.test/api/relay", "peer_id": "Qm…" }
}
```

`peer_id` (relay and registration; multi-relay [MR008](../../projects/multi-relay/DECISIONS.md#mr008--a-relay-address-carries-its-peerid-no-first-use-trust)):
the PeerId that relay must answer to — its address carries it. `prod` / `qa` take the build's
(`PP_RELAY_PEER_ID_PROD` / `PP_RELAY_PEER_ID_QA`, [BUILD](../ops/BUILD.md#backend-origin)) and ignore the saved
ones; `custom` takes the config's, `PP_BROWSER_ORIGIN` takes `PP_BROWSER_RELAY_PEER_ID`. Me → Network's custom relay
and registration fields show and take relay links (`<base_url>#<peer_id>`); a bare URL there names none. A custom
relay (or `PP_BROWSER_ORIGIN`'s) none names is not used in a release build — Me → Network says its link has no relay
ID, and an ERROR names the missing setting — and pinned on its first identity answer, warned, in a developer build
([§ Pins](#pins)). The directory is unsigned and needs none.

`directory.providers[]` (N029 nd3) is an **ordered failover list**. When present and non-empty, `CreateOrgBackendClients` builds HTTP clients for each `transport: http` entry and wraps them in `FailoverDirectoryClient`. When `providers` is omitted/empty, behavior matches legacy single `directory.base_url`. Non-`http` transports (e.g. future `amp`) are skipped with a warning until Phase B.

`transport` on relay/registration remains reserved (`http` now). Directory providers use the same vocabulary.

## HTTP registration (challenge + sign bytes)

When `registration.base_url` is set (e.g. `https://host/api/relay`), `HttpRegistrationClient` uses a 2-step challenge flow:

| Step | HTTP | Request body | Response |
|------|------|--------------|----------|
| Start | `POST /v1/register/start` | `{ public_key, kem_public_key_b64, nickname?, signature_alg? }` | `{ challenge, signature_alg, expires_at }` |
| Finish | `POST /v1/register/finish` | `{ challenge, public_key, kem_public_key_b64, kem_binding_signature_b64?, sign_version, audience, signature, timestamp, nickname?, signature_alg?, account_card? }` | `{ success, account_id, registration_id, message, expires_at, llm_api_key, kem_bound, account_card_status? }`; `426 client_upgrade_required` for an older sign version or account card ([Client compatibility](#client-compatibility-discovery)) |
| Revoke | `POST /v1/register/revoke` | `{ account_id, registration_id, sign_version, audience, timestamp, signature }` | `200`; `409 registration_mismatch` when another registration is held now (nothing removed) |

Finish signs canonical bytes: domain `pp-browser:relay-register-v1\0`, `sign_version=3`, the relay's PeerId (`audience`, len-prefixed UTF-8, [§ The audience](#the-audience-every-signed-command-names-its-relay)), challenge (len-prefixed UTF-8), **1952-byte raw ML-DSA-65** account public key, **1184-byte raw ML-KEM-768** **account** public key (`kem_public_key_b64` — person encapsulate-to, **M015**; not per-device), `signature_alg` u8 (`1=ml-dsa-65`), `timestamp` i64 BE. Pre-release hard cut: Ed25519 register/API auth removed; wipe legacy relay_users.

Brief derives `account_id = account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 pk))>` and keeps **one row per Account ID** (M006): the Account ID is the row's key and the account's inbox address — the relay issues no ids of its own ([multi-relay MR001](../../projects/multi-relay/DECISIONS.md#mr001--inboxes-are-keyed-by-account-id)). Finish answers with it (`account_id`), and the client refuses a registration answered for another account. It carries the account's signed card (`account_card`, version 4, [ACCOUNT_CARD.md](ACCOUNT_CARD.md); a pp-node sends none): the relay checks it as the app does — a card that does not verify, is another account's, or breaks a bound refuses the registration (`400` `invalid_account_card` / `account_card_mismatch` / `account_card_too_large` / `invalid_card_relays`) — keeps the newest and serves it with the directory row (`account_card_status`: `stored` / `same` / `stale`). The relay reads the account's **initiation floor** from it (the stranger check below); there is no registration field for the floor ([pricing P001](../../projects/pricing/DECISIONS.md#p001--initiation-floor--media-quote-gates-no-payment-rails-yet) item 7 as amended by [multi-relay MR005](../../projects/multi-relay/DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant)). The app registers again, whatever the auto-renew preference, when its card is newer than the one the relay last answered for (`registered_card_issued_at` in the identity; `ShouldPublishAccountCard`), so a changed floor, relay list or device list reaches the relay on the next start. An id of another shape is refused everywhere with `400 invalid_account_id`. Directory / user lookup include `account_id`, `signature_alg=ml-dsa-65`, and the **account** `kem_public_key_b64` (same key on every linked device) with `kem_binding_signature_b64`: the account's signature binding that KEM key to it ([MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md), `pp-browser:kem-binding-v1`). Finish verifies a binding it is sent and stores it with the KEM key, replacing both (none sent, none kept); `kem_bound` says the entry now carries one. Clients take a directory KEM key only with a valid binding, and a registration from before bindings re-registers once to publish one.

## Relay identity and the audience (multi-relay MR007)

A signed relay command is good at **one** relay: it names that relay's PeerId, so a relay on a user's list cannot
replay the user's signatures at the user's other relays ([MR007](../../projects/multi-relay/DECISIONS.md#mr007--relays-have-a-node-style-identity-every-signed-relay-command-names-it-as-its-audience);
the threat: a relay, honest-but-curious or hostile, not the network — TLS covers that).

### Relay identity

Each relay deployment holds its own ML-DSA-65 key; its **PeerId** is `pp::PeerIdFromMlDsaPublicKey(key)`, exactly a
pp-node's ([MESH_IDENTITY](../architecture/MESH_IDENTITY.md): base58btc of `12 20 ‖ SHA-256(08 04 12 A0 0F ‖ key)`,
`Qm…`, 46 characters). A new key is a new relay identity — and so a new relay address.

**A relay address is its origin and its PeerId** ([MR008](../../projects/multi-relay/DECISIONS.md#mr008--a-relay-address-carries-its-peerid-no-first-use-trust)),
as a node's multiaddr ends in `/p2p/<PeerId>`. Its written form, the **relay link**, is
`https://host[:port][/api/relay]#<PeerId>` (`ParseRelayLink`, `RelayLink`): the fragment is the base58 PeerId and is
never sent to the relay. An operator publishes it (pp-relay-apis' `scripts/relay-identity-keygen.mjs <origin>` prints
it; a local stack's `/_local/status` names it, `relayLink`). The app never learns a PeerId from a relay's own answer.

`GET {base}/v1/relay-identity` (unauthenticated, rate-limited per address) answers
`{ "schema_version": 1, "peer_id": "Qm…", "public_key_b64": "…", "signature_alg": "ml-dsa-65" }` with
`Cache-Control: public, max-age=300`; a relay without a valid identity answers it, and every signed call, `503
relay_identity_unavailable`. The client (`FetchRelayIdentity` / `ParseRelayIdentity`) takes it only when
`schema_version` is 1, `signature_alg` is `ml-dsa-65`, `public_key_b64` is canonical base64 of 1952 bytes and the key
derives `peer_id`; anything else (and unreachable, any other status) is no identity. Unknown keys are ignored. The
answer is **checked, not trusted**: its `peer_id` must be the one the relay's address names, else the relay is not
used. It is not signed: a static signature is copied by whoever serves it, and a relay proxying another answers its
nonce through.

### Pins

The pins are the identities we expect (`RelayPins` over `RelayPinStore`, `relay_pins.json`
[DATA_LAYOUT](DATA_LAYOUT.md#relay_pinsjson)): each relay of ours and each of the org's origins with the PeerId its
address names. One PeerId per origin, one origin per PeerId, at most 32 (the oldest pin that is neither ours nor the
org's origins goes first). The app signs for a relay only the PeerId its pin holds.

- **Where the PeerId comes from** (MR008 item 2), never from the relay's answer:
  - the org's relay (`relay`, and `registration` where it is another origin): the build's beside its origin
    (`PP_RELAY_PEER_ID_PROD` / `PP_RELAY_PEER_ID_QA`) for `prod` / `qa`, the config's `peer_id` for `custom`;
  - a relay of ours: the address it is kept with in My relays (`our_relays.json` `peer_id`) — the relay link it was
    added by, the org's above for the seeded one, the relay links of `relay.extra_base_urls`, our own verified card's
    entries on a linked device (which also replace a pin that disagrees and drop another origin's pin of that PeerId),
    the account locker's on a restored device;
  - a contact's relay: its newest card's `{origin, peer_id}` (version 4); a contact added by QR code before its card:
    the locator's `relay` `{origin, peer_id}`. These are signed for as they name them and never pinned
    (`CheckCardRelay`, [§ Sending by the list](#sending-by-the-list)).
- **Uniqueness stops a relay fronting another:** an address naming a PeerId one of our pins holds at another origin is
  refused — at Add (`relay_identity_taken` / `relay_identity_taken_known`), when the list or config names it
  (`Expect`: that relay is not used, logged), and for a contact's entry (skipped). The org's pins are written first,
  so a link naming the org's PeerId can never take it, and a hostile relay that answered first wins nothing.
- **Checked, not learned:** at start, a list change, the network back and the app in front, each relay of ours and
  each of the org's origins is asked for its identity once per run (`CheckIdentities`; nothing waits for it: the
  expected PeerId is signed for meanwhile). So is a relay that answered `401 wrong_audience` (at most once per origin
  per 5 min). An answer naming another PeerId: the relay **does not match its address** — kept as
  `presented_peer_id`, logged as an error; nothing is signed for it (it is not polled, registered on or sent to, and
  left off our card, re-issued at once) until a later check finds it answering to its address again. There is no
  "accept": a rotated key is a new address, which the person adds by its new link (and the org's ships with a new
  build). The same PeerId after `wrong_audience`: a misbehaving relay — the call fails like a 5xx, logged.
- **First use, developer builds only** (MR008 item 4): a developer build (`developer_relays`) may add a bare origin,
  and seed one from `relay.extra_base_urls`; such an origin is pinned on its first identity answer (logged as a
  warning), at Add or before its first signed call; so are the org's origins with none named. Outside developer
  builds only the compiled `prod` / `qa` backend of a build that ships no PeerId (a dogfood build: a release workflow
  refuses an empty `PP_RELAY_PEER_ID_PROD`) is pinned so, warned (`OrgRelayFirstUseAllowed`); a release build's custom
  backend or `PP_BROWSER_ORIGIN` without `relay.peer_id` (or `registration.peer_id` / `PP_BROWSER_RELAY_PEER_ID`) is
  not used (an ERROR names the setting; Me → Network says so). Any other origin no address names gets nothing signed
  (`relay_identity_unavailable`). A first-use
  fetch that fails pins nothing; it is asked again after 1 min, doubling to 2 min, and at once when the network comes
  back, the app comes to the front or the list changes.
- Our card lists each relay of ours whose pin is in force; while a check pass runs, the card waits for its end and is
  re-issued once, so a linked device does not follow a list one check at a time.
- **pp-node** expects its `registration.base_url` to answer to `registration.peer_id` (`PP_NODE_REGISTRATION_PEER_ID`;
  with the compiled prod registration URL, the compiled `PP_RELAY_PEER_ID_PROD`; [CONFIGURATION](../ops/CONFIGURATION.md#pp-node-relay-identity)),
  kept in its data directory (`relay_pins.json`, the same format); the compiled URL is matched however it is spelled
  (trailing `/`, case, default port). With none named a release build does not register there or send heartbeats (an
  ERROR names `registration.peer_id (PP_NODE_REGISTRATION_PEER_ID)`, and each publish fails with it); a developer
  build pins that relay's first answer, logged as a warning. An identity answer naming another PeerId (after a `wrong_audience`) stops its relay calls
  (registration, heartbeat) and is logged as an ERROR (`relay identity mismatch: expected <X>, presented <Y>`) until
  the relay answers to its address again or the operator names its new PeerId; the mesh keeps running.

**What remains trusted** (MR008 item 6): a link's source. A relay operator controls what it publishes, so a hostile
operator can publish its own origin with another relay's PeerId and pass that relay's answers through. Uniqueness keeps
it from fronting the org or any relay of ours (no org secrets reach it: the Brief LLM key comes only from a registration
at the org's pinned origin); it can at most front an unrelated relay we hold no identity for, until requests are sealed
to the relay's key (Amp, MR007 item 6).

### The audience: every signed command names its relay

Each command's sign bytes are `domain ‖ 0x00 ‖ u8 version ‖ LenUtf8(audience) ‖ …its fields…`, the audience the
relay's PeerId string (`LenUtf8` = `u64be(len) ‖ utf8`, as every string here; integers big-endian). The JSON body
carries it as a top-level `"audience"` beside `sign_version` (the version byte the bytes carry; `SignRelayCommand`
reads it from them, `PutSignedRelayCommand` writes it), `timestamp` and `signature`. The relay checks the sign version
**first**, from the body alone: none, or one older than the table's, answers `426 { "code": "client_upgrade_required" }`
([Client compatibility](#client-compatibility-discovery): this app is too old for that relay), a newer or malformed one
`400 unsupported_sign_version`. It then refuses an `audience` that is absent or not byte-equal to its own PeerId with
`401 { "code": "wrong_audience" }` (before the window, the account lookup, the signature, and for `register/finish`
before the challenge is spent), then verifies the signature over bytes built with its own PeerId: a body whose
`audience` was rewritten fails the signature. `register/finish` answers `426 client_upgrade_required` to a person's
account card older than version 4 as well, before anything is spent.

| Domain | Version | Fields after the audience |
|---|---|---|
| `pp-browser:relay-register-v1` | 3 | `challenge`, account ML-DSA-65 key (1952 raw), account ML-KEM-768 key (1184 raw), `signature_alg` u8 (`1`), `timestamp` i64 |
| `pp-browser:relay-revoke-v1` | 2 | `account_id`, `registration_id`, `timestamp` |
| `pp-browser:relay-profile-v1` | 2 | `account_id`, `nickname`, `timestamp` |
| `pp-browser:relay-profile-icon-v1` | 2 | `account_id`, `url`, `blob_id`, `kind`, `timestamp` |
| `pp-browser:relay-heartbeat-v1` | 2 | `account_id`, `peer_id`, `state`, `timestamp` |
| `pp-browser:relay-llm-token-v1` | 2 | `account_id`, `timestamp` (the app does not call `/api/llm/v1/token/renew` yet) |
| `pp-browser:relay-locker-put-v1` | 2 | `account_id`, `locker_id`, `version` u64, `timestamp` |
| `pp-browser:relay-blob-presign-v1` | 2 | `account_id`, `content_type`, `byte_length` u64, `purpose`, `timestamp` |
| `pp-browser:relay-blob-retain-v1` / `-blob-delete-v1` | 2 | `account_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-blob-list-v1` | 2 | `account_id`, `status_filter` (`""`, `pending`, `retained`), `timestamp` |
| `pp-browser:relay-file-start-v1` | 2 | `account_id`, `byte_length` u64, `timestamp` |
| `pp-browser:relay-file-part-v1` | 2 | `account_id`, `blob_id`, `part_number` u64, `timestamp` |
| `pp-browser:relay-file-status-v1` / `-complete-v1` / `-delete-v1` | 2 | `account_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-file-list-v1` | 2 | `account_id`, `status_filter`, `timestamp` |
| `pp-browser:relay-api-v1` | 3 | `op` u8, `timestamp` i64, then the op's fields ([below](#http-relay-api-auth-per-request-sign-bytes)) |

Domain names keep their `-v1` suffix (the u8 is the version). No other version is accepted (a clean cutover,
[COMPATIBILITY](COMPATIBILITY.md#clean-cutovers-pre-release)): an older one, or none named, is `426
client_upgrade_required`, so an app the relay has moved past says "update the app" rather than failing as a bad
signature. **Statements verified anywhere carry no audience:** the
KEM binding, the account card, grants, the account locker's owner signature, and everything end to end inside a
message.

**Where each client takes its audience** (`IRelayAudience`; every client signs through `SignRelayCommand` and reports
refusals through `NoteRelayRefusal`): the clients of a relay of ours (messages, push devices, registration, blobs and
files) its pin; the lockers at the org's `registration.base_url` that origin's pin (pinned whether or not it is ours);
a send into a contact's relay the `peer_id` its newest card (or its QR locator) names for that relay
([Sending by the list](#sending-by-the-list)); pp-node its relay's pin.

**Golden vectors:** `capabilities/delivery/src/lib/delivery/tests/data/relay_sign_vectors.json`, written by
`relay_sign_vectors_test.cpp` (`PP_WRITE_RELAY_SIGN_VECTORS=<path>`) and checked there: every domain and relay API op
signed by one account for relay `a`, with what `a` and `b` answer (`ok`, `wrong_audience`, `bad_signature`,
`client_upgrade_required`), a poll without `audience` and one whose `audience` names `b`, a poll, a registration, a
revoke and a heartbeat naming an older `sign_version` and a poll naming none, the relays' keys and PeerIds, MESH_IDENTITY's two fixtures, a
version 4 account card and five cards that must be refused. pp-relay-apis copies the file byte for byte
(`relay-sign-vectors.json`); a change to either side's bytes changes it here first.

### Commands repeated late

A command of our own app may arrive twice or late (a retry, a request held up to the ±5 min window). Every command is
safe so (dedup, versions, single-use challenges, strictly increasing timestamps, unique blob ids, reads) but two,
which name what they act on:

- **`register/revoke`** names the `registration_id` `register/finish` answered (opaque, 16–64 `[A-Za-z0-9_-]`, new for
  a fresh registration, the same across its renewals). The app keeps it with each registration: the identity's
  (preferred relay) and each other relay's record in `our_relays.json`. Another id held there: `409
  registration_mismatch`, nothing changed. A revoke with no id kept, or answered `registration_mismatch`, renews first
  (`register/start` + `finish`, which answers the current id) and revokes that one, once (`RevokeRegistrationWithIdentity`).
- **`devices/unregister`** names the `binding_id` `devices/register` answered (new at every register). The app keeps the
  newest per relay in memory (`HttpPushDeviceClient`; it registers at every start); none kept, or `409
  binding_mismatch`: it registers and unregisters with that answer, once. No row: `200`.

### Self-heal

A signed call to a relay of ours answered `403` / `404` `not_registered` or `registration_expired` while we hold a
registration there we believe current makes it due now: another relay's through `RelayRegistrationsWorkflow::Lost`,
the preferred relay's — the identity's — whatever the auto-renew preference (the person registered there already),
then push binds again. Lost again within 30 min of the last loss: after a backoff of 1 min doubling to 30 min. Not for
a contact's relay (a send keeps its recipient semantics), a relay leaving the list, or one that does not match its
address.

## HTTP relay API auth (per-request sign bytes)

All relay API calls require `sign_version` (3), `audience`, `timestamp` and `signature` over `pp-browser:relay-api-v1\0` canonical bytes, **sign version 3** (`u8 3`, the relay's PeerId as LenUtf8 ([§ The audience](#the-audience-every-signed-command-names-its-relay)), then `u8 op`, `i64 timestamp`, the fields; strings `u64be(len) | utf8`): every party is an **Account ID** (multi-relay MR001). The account key signs; a call by an account not registered here (or expired) is `403 not_registered` / `registration_expired` — except a send, below. Golden vectors: `relay_sign_vectors.json` (above) and `relay_message_sign_payload_test.cpp`.

| Op | HTTP | Body ids | Signed fields |
|----|------|----------|----------------|
| send (0) | `POST /v1/messages` | `sender_account_id`, `recipient_account_id` | sender_account_id, recipient_account_id, stream_id, index_key |
| poll_inbox (1) | `POST /v1/inbox/poll` | `requester_account_id` | requester_account_id, cursor (`""` when none) |
| stream_history (2) | `POST /v1/streams/messages/query` | `requester_account_id`, `sender_account_id` | requester, sender, stream_id, optional index range, limit, order |
| device_register (3) | `POST /v1/devices/register` | `account_id` | account_id, platform, device_id, push_token |
| device_unregister (4) | `POST /v1/devices/unregister` | `account_id`, `binding_id` | account_id, platform, device_id, binding_id ([Commands repeated late](#commands-repeated-late)) |
| ack_inbox (5) | `POST /v1/inbox/ack` | `requester_account_id` | requester_account_id, cursor |
| clear_inbox (6) | `POST /v1/inbox/clear` | `requester_account_id` | requester_account_id, before_created_at (ISO-8601) |

`blob_b64` is **not** included in transport auth (E014 envelope signature covers message integrity inside the blob). Records come back as `{ sender_account_id, stream_id, index_key, blob_b64, created_at }`; the envelope inside must name that same sender (`sender_contact_id`), else the record is skipped.

### A send: the recipient, the sender's key, the grant

`POST /v1/messages` `{ sender_account_id, recipient_account_id, stream_id, index_key, blob_b64, ttl_s?, urgent?, wake_hint?, sign_version, audience, timestamp, signature, sender_public_key_b64?, grant? }` (`HttpRelayClient::Send`, [MR003](../../projects/multi-relay/DECISIONS.md#mr003--cross-relay-delivery-is-an-account-signed-send) / [MR005](../../projects/multi-relay/DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant)). Neither the key nor the grant is signed by the sender: the key must hash to `sender_account_id`, the grant carries the recipient's signature.

- **`grant`** — the recipient's grant to us ([ACCOUNT_CARD § Relay grants](ACCOUNT_CARD.md#relay-grants)), attached when we hold a usable one; never on a send to our own mailbox. With it the send is a contact's; without it, a stranger's.
- **`sender_public_key_b64`** — our account's ML-DSA key, sent only when the relay asked for it (`401 sender_key_required`: it holds no key of ours — we are not registered there); the client sends once more with it.

| Answer | `code` | The client |
|--------|--------|------------|
| `404` | `recipient_not_registered` | Not on this relay (`Err::Network::RecipientNotRegistered`): the next relay of the recipient's list; on every relay of it, the recipient's newest card and its list once more ([Sending by the list](#sending-by-the-list)); then final (the notice "no longer on your message server"). A record that should reach every device names the peer as its Amp fallback: it goes over a link to it when one can be had |
| `401` | `sender_key_required` | Sends once more with `sender_public_key_b64` |
| `401` | `grant_expired`, `invalid_grant` | Drops the grant, asks the recipient for a new one (`relay_grant_ask`), and sends once more without it, as a stranger's |
| `401` | `sender_key_mismatch`, other | Final (`HttpRejected`) |
| `402` | `payment_required` (`initiation_floor`) | Final until paid (`Err::Network::PaymentRequired`, the floor in its detail): the floor is recorded for the recipient ([pricing P001](../../projects/pricing/DECISIONS.md#p001--initiation-floor--media-quote-gates-no-payment-rails-yet)) — the chat shows the price and holds sends while no payment can be made. The relay's price is the authority: it is shown over the recipient's kept card until a newer card is kept, and when that card says another price, or none is kept, the card is fetched again — its devices, then the directories ([The stranger's price](#the-strangers-price)) |
| `429` | `stranger_quota_exceeded` | Queued and sent again after a backoff (1 min, doubling to 16; at most 8 tries, `StrangerQuotaRetryAfter`) |
| `429` | `rate_limited` | Queued, retried when the inbox syncs (as any rate limit) |

The recipient's floor is the `initiation_floor` of the card it registered with (0 when it sent none).

### The stranger's price

The app decides a stranger's price before a first message or call (the chat's compose gate and its "price" line, the
outbound gate, a call's invite offer; `InitiationBillingStore::Get`, `PeerInitiationFloor`) from, in order:

1. the relay's `payment_required` floor, when it came after the newest card we keep for the account (`SetRelayFloor`;
   the relay reads the card the account registered, which may be newer than ours);
2. else the `initiation_floor` of that newest kept card (`KeptInitiationFloor`): one the account sent, a contact card,
   the card a directory lookup carries ([The directory on each](#the-directory-on-each));
3. else, with no card known, the floor heard otherwise: a directory search hit's `initiation_floor` (relays no longer
   send one: 0) or the peer's `charge_required`.

Kept per account in `initiation_billing.json` (the relay's floor beside the others, [DATA_LAYOUT](DATA_LAYOUT.md)).

A stranger's send to a recipient whose floor is 0 is accepted within a first-contact quota (3 an hour per pair, 30 an hour per recipient) and raises **no push** on the recipient's devices (an `urgent` one still rings, within the per-pair call-wake limit).

A poll from a user the relay has no registration for is answered **403** `{"error":"Relay user not registered"}` (observed on the 2026-11 production server for accounts registered on the old one). The client types exactly that answer as `Err::Auth::NotRegistered` (`IsRelayUserNotRegistered`), marks its registration expired and registers again with the same account key (same Account ID; the relay may give a new `relay_user_id`), at most every 5 minutes, when "renew automatically" is on; otherwise it asks the user (B216). Any other 403 stays `HttpRejected`.

Inbox is a **delivery queue**: poll advances a **per-device** local cursor; `ack` is **soft** (M013 — validate only; no shared delete). `clear` deletes recipient rows older than a timestamp (recovery; account-wide). Messenger also applies a **90-day TTL** on `created_at` (startup rewrites the TTL index only if needed) and a **soft per-recipient FIFO cap** (trim toward **2000** when a mailbox exceeds **~2400**, M022). A record sent with `ttl_s` ([WIRE_SCHEMAS § RelayWireRecord](WIRE_SCHEMAS.md#relaywirerecord-http-relay--opaque-store-and-forward)) gets its own expiry instead, and the cap neither counts nor trims it: the control lane's records leave after an hour rather than 90 days, and a burst of them cannot push chat out. Messenger stores the blob as bytes (a third smaller than its base64). Chat history remains local + stream/P2P sync — not the inbox.

Poll response extras (unix ms, relay clock):

- each inbound record may include `created_at` (when the relay stored the row)
- poll body includes `server_time` (relay clock at response build; present on empty polls too)

Call invite age uses `server_time - created_at` when both are present (caller create ≈ relay store).

## Several relays

An account may be on several relays (multi-relay R2; [DESIGN](../../projects/multi-relay/DESIGN.md#app)). Ours are the
profile's list, **My relays** (`our_relays.json`, [DATA_LAYOUT](DATA_LAYOUT.md); Me → My relays, [below](#my-relays)),
preferred first, at most 4, none twice. A new profile's list is seeded once from the deployment's relay
(`relay.base_url`, the preferred one) and the other base URLs of `relay.extra_base_urls`, in that order
([CONFIGURATION](../ops/CONFIGURATION.md); `OurRelaysStore`, `OurRelayOrigins`, `BackendClientSet`); afterwards the
config is only a new profile's default. Our account card lists the `https://` ones, preferred first
([ACCOUNT_CARD](ACCOUNT_CARD.md#account-card)).

### Relay services and org services

Two origins ([multi-relay MR006](../../projects/multi-relay/DECISIONS.md#mr006--the-preferred-relay-carries-the-users-relay-services-org-services-stay-on-the-orgs-origin);
`BackendClientSet`): the **org's** (the deployment's origin: the compiled one for `prod` / `qa`, the config's
`relay` / `registration` for `custom`) and **the preferred relay** (the first of My relays). They are the same until the
user changes the list.

| Follows the preferred relay | Stays on the org's origin |
|---|---|
| The inbox (polled at pace; the others more slowly), push tokens, registration (the identity's) | Client compatibility (`/v1/client-compat`) and with it Support (`support`) and the crash-report URL it may give |
| The directory: search and lookups ask the preferred relay's first, then our other relays' ([below](#the-directory-on-each)) — unless `directory` is configured apart from the org's relay | Crash reports (`/v1/crash-reports`), diagnostics (`/api/support/diagnostics`) |
| Profile photos and blobs ([HTTP relay blobs](#http-relay-blobs-profile-photos)), chat attachment files ([HTTP relay files](#http-relay-files-chat-attachments)) | The Brief LLM gateway (`/api/llm/v1`) and its key, the promoted MCP server (`/mcp`) |

- **The Brief LLM key is the org's:** only a registration at the org's relay keeps the `llm_api_key` it answers with
  (`HttpRegistrationClient::SetTakesBriefLlmKey`); when the org's relay is another of ours, its registration
  (`RelayRegistrationsWorkflow`) gives the identity the key. Another relay's key is not kept.
- **Not moved (yet):** account lockers stay at the org's registration URL ([HTTP account lockers](#http-account-lockers-multi-device-m023)),
  where every locker so far was written and where a restoring device, which knows no list yet, looks. The app keeps
  writing them there whatever the list; with the org's relay removed from it our registration there is revoked, so
  those writes fail (logged: the relay takes a locker write from a registered account only) until a later step moves
  them, while a restore still reads them (by locker id). Stream history is asked of the org's relay while it is one of
  ours (`HistoryRelay`), else of the preferred one (what the org's relay holds is then not asked).

### Registering and receiving on each

- **Registration.** The preferred relay's is the identity's, as above (the person registers; it renews there). Once it
  is registered, the app registers on each other relay of ours too, with the same card at `register/finish`, and again
  when the card is newer than the one that relay answered for, when its registration is within 14 days of its end, or
  after a failure (1 min, doubling to 30 min) — `RelayRegistrationsWorkflow`, checked every minute while messaging
  runs. What each relay answered (registered until, or failed and why) is kept with the list (`our_relays.json`) and
  given back at the next start: a relay that holds our registration for the current card is not asked again, and its
  inbox is polled at once. Revoking (a profile reset) revokes on each.
- **Polling.** Each inbox has its own cursor (`relay_inbox_cursor.json`, [DATA_LAYOUT](DATA_LAYOUT.md)) and one poll
  at a time (`RelayInboxCoordinator`). The preferred relay is polled at the app's pace
  ([THREADING](../architecture/THREADING.md): ~2 s in the foreground); the others at every **4th** sync (~8 s), and
  only once they hold our registration. A forced sync — a push or a wake — polls every relay: any of them may hold
  the mail that woke us. A batch is acked on the relay it came from; `clear` clears every inbox.
- **Twice.** A message that reaches us through two relays (a sender that tried a second after a timeout, a copy
  for our other devices) is taken in twice: the receive pipeline drops the second by its message id, as any
  redelivery. One relay batch is taken in at a time.
- **Push.** The push token is registered on each relay that holds our registration ([HTTP device push](#http-device-push-opaque-wake)).
- **Each inbox's cursor is its relay's** (`cursor_relay` names the preferred one's): a relay made preferred polls on
  from its own, and a relay that leaves the list loses its cursor.
- **The preferred relay only:** profile photos and blobs, chat attachment files ([Relay services and org services](#relay-services-and-org-services)).

### My relays

Me → My relays changes the list (`MyRelaysWorkflow`; the page in [UI_DESIGN_SYSTEM § My relays](../ui/UI_DESIGN_SYSTEM.md#my-relays)).
Only the account's main device changes it (it signs the card's list, MR002); a linked device follows the newest card
of ours (its list becomes the card's). Every change is kept first, then:

1. the clients, the inboxes and the card's relays follow the new list (on the hub's thread; when it does not take the
   list within 10 s, or the profile closes meanwhile, the change is undone and refused with `relay_list_not_applied`:
   nothing is registered or announced for it);
2. what is new is registered on (each other relay again for the newer card, as above);
3. the card is re-issued with the new list (our other devices hear through our mailbox) and sent to every contact
   with the device-list notice over the control lane (`AnnounceOwnDevicesToContacts`), so senders deliver by the new
   order; a contact that still holds the old card writes into a relay of the old list, which we still poll while it
   is ours.

| Change | Rules | Also |
|---|---|---|
| **Add** | A **relay link** (MR008), pasted or scanned from a QR code: `https://host[:port]`, or that and `/api/relay`, then `#<PeerId>` (no other path, query or user info; the origin at most 128 bytes): a link with no PeerId is `relay_link_no_id`, one whose fragment is no PeerId `relay_origin_bad_relay_id`. A bare origin (no `#`), `http://`, and a host that is not public (on this device or a private network: `IsPublicRelayOrigin`, the card relays' policy, [ACCOUNT_CARD](ACCOUNT_CARD.md); refused with `relay_origin_local` before it is asked), only in developer builds (never on the card; a bare origin is pinned on its first answer, [§ Pins](#pins)). Outside developer builds every request to it — Add's two asks and, once added, all its clients' (and its identity checks) — goes over https to a public address only, checked at connect time, so a name that resolves inside later is refused; the relays the deployment configures are not held to it. A link naming a PeerId one of our pins holds at another origin is refused before it is asked: `relay_identity_taken` (a relay on our list) or `relay_identity_taken_known` (the org's origin); an origin we expect to answer to another PeerId (the org's) is `relay_identity_mismatch`. It must answer `GET <base>/v1/client-compat` (the cheapest unauthenticated GET every relay serves) and `GET <base>/v1/relay-identity` with the link's PeerId: no identity is `relay_identity_unavailable`, another PeerId `relay_identity_mismatch` (nothing kept). At most 4. | Added last, kept with the link's PeerId (`our_relays.json` `peer_id`), which is expected from then on |
| **Remove** | Not the preferred relay (make another preferred first), never the last (mesh-only is not offered in this step) | Our push binding (`/v1/devices/unregister`) and our registration (`/v1/register/revoke`, which also drops the account's push devices there) go first, best effort — none for a relay that does not match its address (nothing is signed for it; its registration expires there); once the services left it its pin goes (its PeerId is free for another address), unless it is the org's origin, whose services still sign for it; its inbox cursor goes. When the services do not follow (the change is undone), the relay is ours again and registered on again |
| **Make preferred** | A relay of ours | The identity's registration renews there at once (the card is newer, so it goes whatever the auto-renew preference; the registration records the relay it was made at, so the Me page reads it as the current one); push binds there; the old preferred relay is registered on as another of ours |

A refused change answers with a code the page words (`relay_origin_not_https`, `relay_origin_local`, `relay_unreachable`,
`relay_link_no_id`, `relay_origin_bad_relay_id`, `relay_identity_unavailable`, `relay_identity_mismatch`,
`relay_identity_taken|<host>`, `relay_identity_taken_known|<host>`,
`relay_already_listed`, `relay_list_full`, `relay_preferred_not_removable`, `relay_list_last`,
`relay_list_not_main_device`, `relay_list_not_applied`, …). A profile closing (a reset, the app stopping) waits for a
change in progress and refuses any after it.

**Our relay links:** each relay of ours with a PeerId shows its link, `<origin>#<PeerId>`, with **Copy link** (any
device): what another person, or another app of ours, adds it by.

### The directory on each

The directory the app asks (`OurRelaysDirectory`, built by `BackendClientSet`) is the configured one (`directory`:
by default the preferred relay's, its `providers` failing over as before; one configured apart replaces the preferred
relay's) and then the directory of each other relay of ours (My relays) it does not name, in our order ([DESIGN § Not in scope](../../projects/multi-relay/DESIGN.md#not-in-scope): a name
search asks the relays the user has listed; there is no search across relays beyond them).

- **A name search** (`GET /v1/search?q=`: the contacts page's *Add contact* and the assistant's `search_people`, both
  on a worker) asks all of them at once: the first on the searching worker, the others on workers of their own, each
  request bounded (10 s, 5 s to connect, on the other relays), and the search waits for them until **10 s** after it
  started. The answers are merged (`MergeDirectoryAnswers`): the first directory's hits in its order, then each next
  one's accounts not seen yet; an account is listed once, and when several answered for it, its entry is the one
  whose keys verify (`DirectoryHitKeysVerify`: the signing key hashes to the Account ID, the KEM key carries its
  binding), else the earlier directory's (the preferred relay's first): an entry's endpoint times are its relay's
  unsigned word, so another relay of ours cannot override the name, photo or addresses by claiming newer ones. The list is then ranked as
  one (exact and prefix name matches first). A directory that fails or does not answer in time adds nothing and fails
  nothing (it is logged); the search fails only when none answered, with the first one's error.
- **A lookup by Account ID** (`GET /v1/users/by-account`: an exact-id search, *Add* / *Message* on a directory row,
  key resolution, the contacts' directory shadows, every lookup of one account) asks them in order, a relay
  that could not be reached in the last 5 minutes after the others (`RelayHealth`), and stops at the first that knows
  the account. Not found where it could ask is not found; reaching none is the first failure.
- **The card a lookup answer carries** (`account_card`) is kept once it verifies and is newer than ours
  (`DirectoryCardSink` → `AccountKeyResolver::AddCarriedCard`, source `directory`; our own account's is left to
  `OwnCards`), so a stranger found in the directory has its relays and its price before a first message.
- **Mesh nodes** (`GET /v1/mesh/nodes`) are the first directory's.

### Sending by the list

`RelaySendCoordinator` (the outbox's relay port) sends a record into its recipient's relays in their list order:

1. **The list:** the relays of the newest card we keep for the recipient (`KeptRelays`); before any card, a contact
   added by QR code has its locator's preferred relay, `{origin, peer_id}` ([ACCOUNT_CARD § Locator](ACCOUNT_CARD.md#locator-qr-code-invite-link),
   multi-relay R4, MR008); a recipient with neither, or an empty list, goes to our preferred relay; a copy for our own
   other devices goes to our relays.
2. **Health:** a relay a send could not reach (a transport failure, a timeout, a 5xx) is tried after the healthy
   ones for **5 minutes** (`RelayHealth`); a send it takes clears it. An answer about the recipient (a price, a quota,
   not registered) says nothing about the relay's health.
3. **The audience** (MR007) is the `peer_id` the recipient's card (or locator, MR008) names for that relay; a relay of
   ours with none named is signed for by its pin (our preferred relay for a recipient without a card); a relay not ours
   with none named gets nothing signed ([§ Pins](#pins)). A card
   entry our pins disagree with — our pin for that origin is another PeerId, or that PeerId is pinned to another
   origin — is skipped for that recipient (logged once per entry; the card is still kept; the contact page reads
   *Not used: it doesn't match this contact's card*). A relay that answers `wrong_audience` is skipped for this send, counts like
   `recipient_not_registered` toward the stale-list fetch (step 5: the contact may have re-issued its card), and,
   while the newest card still names the refused PeerId, is skipped for that recipient for 5 minutes. Neither marks
   the relay unhealthy; a relay of ours that does not match its address is skipped alike.
4. **The next relay** is tried on a transport failure or a 5xx, and on `recipient_not_registered`. Any other refusal
   (`payment_required`, `stranger_quota_exceeded`, `rate_limited`, another 4xx) is the send's answer. When every relay
   failed, the transport failure is reported (the send is retried later), else `recipient_not_registered`.
5. **A stale list:** `recipient_not_registered` (or `wrong_audience`) on every relay tried fetches the recipient's newest card — its devices
   over the mesh (8 s at most), then the directories of our relays and of our other directory providers
   (`GET /v1/users/by-account`'s `account_card`), kept only when it verifies (`RecipientRelays`) — and tries its list
   **once** more when it names other relays. At most one fetch per recipient a minute; a send that finds the list
   stale while another fetches it waits for that fetch (12 s at most) and takes its list.

A relay of ours is sent to as a registered sender; any other relay gets a client of its own (at most 16 kept), and
our key when it asks for it (`sender_key_required`). The grant is the recipient's, the same on every relay.

### A contact's relays, by Account ID

The contact page's *Refresh account info* (after the contact's devices) asks the contact's relays in list order, then
ours, by `GET /v1/users/by-account/{account_id}` — unsigned, so a relay we are not registered on answers as any
other — up to the first that keeps the account; a transport failure, a 5xx or a `404` gives way to the next. The
row's keys and its `account_card` are kept only when they verify against the Account ID
([ACCOUNT_CARD § Refresh on the contact page](ACCOUNT_CARD.md#card-exchange-over-amp)); what each relay answered
is the reachability the page shows for the contact's preferred relay (in memory, until the next refresh).

## Directory and profile

| HTTP | Purpose |
|------|---------|
| `GET /v1/search?q=` | Search **people** only (`entity_kind` person / missing). Matches nickname (case-insensitive) or Account ID (exact). Each hit's `hit_id` and `account_id` are the Account ID; `ids` list it (`kind: account`, primary) and the endpoints' `peer_id`s. Hits include `endpoints[]`, keys, optional `entity_kind`. **Excludes** `mesh_node` ([N027](../../projects/p2p-mesh/DECISIONS.md#n027--mesh-directory-entity_kind-pluggable-providers-bootstrapdirectory)) |
| `GET /v1/mesh/nodes` | List non-expired **`mesh_node`** infra listings: `account_id` (the row's key), `nickname`, `endpoints[]`, `capabilities` (`circuit_relay` / `media_relay`), `expires_at`, keys; and, when the node registered them, what it runs: `amp_wire`, `node_version`, `features[]` (N032, below). A row without an `account_id` is skipped |
| `GET /v1/users/by-account/:account_id` | Lookup by Account ID (any kind): `account_id`, keys, `signature_alg`, nickname, expires_at, **`endpoints[]`**, optional `entity_kind` / `capabilities`, and the newest `account_card` the account registered with. `404 not_registered`: not on this relay (or expired). The client takes a row only for the account it asked for. There is no lookup by any other id |
| `POST /v1/profile/nickname` | Update nickname (`relay-profile-v1` sign bytes + signature) |
| `POST /v1/register/start` | Start registration (`public_key`, `kem_public_key_b64`, optional `nickname`, `peer_id`, `multiaddrs`, optional `entity_kind` / `capabilities`) |
| `POST /v1/register/finish` | Finish/renew (`entity_kind` default `person`; `mesh_node` for pp-node). Upserts `endpoints[]` by Peer ID; persists capabilities for mesh nodes, and the node's `amp_wire` (int), `node_version` (string) and `features` (string[]) when sent (N032); echoes **`account_id`** |
| `POST /v1/register/revoke` | Remove this account's registration and its push devices (profile reset / delete account, #328). Body `{ account_id, timestamp, signature }`; signature over `pp-browser:relay-revoke-v1\0`, `u8 1`, `account_id` (len-prefixed UTF-8), `i64 timestamp`, by the account key. Best effort on the client: a failure does not block the local reset |

**Directory providers:** Brief HTTP is the default (`directory.base_url`). Clients should treat directory as pluggable (N027); bootstrap peers remain L0 cold-start / emergency dial, not the long-term sole mesh-service discovery path.

**What a mesh node runs** ([N032](../../projects/p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes)): pp-node registers `amp_wire`, `node_version` and `features` at the top level of `register/finish`, and the directory lists them on its `/v1/mesh/nodes` row, verbatim. They are the node's own claims: a client drops a row whose `amp_wire` is not its own before dialing (`WithoutOtherWireVersions` in the directory cache), and never ranks a node higher for them. A row without them (a node or directory before N032) is dialed as before. A person's registration carries none of them. Between peers, a wire mismatch shows as amp's version negotiation ([ADP.md](ADP.md)); other app-version differences stay out of the directory and show over messaging (soft-skip, protocol ids).

## HTTP relay blobs (profile photos)

Base path under the preferred relay's base URL ([Relay services and org services](#relay-services-and-org-services); e.g. `https://backend.example.test/api/relay/v1`). Client: `HttpBlobClient` / `IBlobClient` ([relay-blob-upload](../../projects/relay-blob-upload/)). **Only profile photos use these routes now** (`purpose=icon`, then `/v1/profile/icon`); chat attachments go through [HTTP relay files](#http-relay-files-chat-attachments) and never call `/v1/blobs/presign`. The `purpose=file` rows below describe what the relay still accepts; the client no longer sends them.

Auth: separate sign domains (not `relay-api-v1`). Canonical bytes via `RelayBlobSignPayload` — domain + u8 version=`2` + the relay's PeerId ([§ The audience](#the-audience-every-signed-command-names-its-relay)), then length-prefixed UTF-8 / BE integers; the body carries `audience`.

| Domain | Signed fields |
|--------|-----------------|
| `pp-browser:relay-blob-presign-v1` | `account_id`, `content_type`, `byte_length` (u64), `purpose`, `timestamp` (i64) |
| `pp-browser:relay-blob-retain-v1` | `account_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-blob-delete-v1` | `account_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-blob-list-v1` | `account_id`, `status_filter` (empty if none), `timestamp` |
| `pp-browser:relay-profile-icon-v1` | `account_id`, `url`, `blob_id`, `kind`, `timestamp` (empty strings when unused) |

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
| 404 `Blob not found` | retain / delete (e.g. the pending upload expired); presign for an account not registered here | `Expired` | failure line; a CDN 404 on download is not an API answer (typed in the download step) |
| 400 `File exceeds size limit` (also 413 if a proxy sends it; the relay itself never answers 413) | presign above `largeMaxBytes` | `TooLarge` | not retried |
| 429 `Small file quota exceeded` / `Large file storage quota exceeded` | presign | `QuotaExceeded` | Shown as the failure. The client presigns profile photos only, which have their own limit, so it does not plan a recovery on `/blobs` (it no longer lists or frees the small / large tiers) |
| any other 429: `Rate limit exceeded` (120 per user per minute), `Too many pending uploads`, an empty or proxy / WAF body | every blobs route / presign | `RateLimited` | "sending too fast", no delete offered |
| other 5xx | gateway / relay failure | `Unavailable` | failure line, retry |
| anything else (400 `Unsupported content type`, 400 `Hosted blob URL does not match`, 401 signature errors, …) | | `Generic` | failure line |

Chat attachments do not use these routes (their failures are in the relay-files table below), so the 429 quota rows above no longer come from a chat send. The space warning reads the files relay's usage instead (`CheckBlobSpace` → `/files/list` `usage`: `bytes_used` of `quota_bytes`, the space attachments fill) after a successful upload, at most once a day and after every quota answer. The client tells the person once at 80 % and once at 90 %.

**Transfer of the ciphertext (client).** The upload `PUT` to the `upload_url` from `files/start` / `files/part` is streamed from a file (`HttpClient::PutFile`, `IBlobClient::PutUploadFile`): no whole-body string, progress reported as bytes go out, no total time limit, but a 15 s connect timeout and a speed floor (under 2 KB/s over 30 s aborts). The download `GET` of the message's relay download link (302 to storage) goes to a temp file (`HttpClient::GetToFile`) with a hard cap of the declared plaintext size plus the 16-byte AEAD tag: a larger `Content-Length` is refused before any byte is read, a body that grows past the cap is aborted and the temp file deleted, and a non-2xx answer writes nothing. `kMaxHttpClientBodyBytes` (8 MiB) still bounds every buffered request.

Chat attachment **file bytes** prefer peer-direct `/pp/blob/1.0.0` then CDN ([R019](../../projects/relay-blob-upload/DECISIONS.md#r019--peer-first-blob-transfer-cdn-secondary)); the small attachment envelope stays on the normal message path.

## HTTP relay files (chat attachments)

The encrypted-file relay (app-static-api `docs/relay-files.md`, routes `app/src/r_relay.ts`), same base path. Client: `IBlobClient::FileStart` / `FilePart` / `FileComplete` / `FileDelete` / `FileList` (`HttpBlobClient`), `UploadRelayFile` / `UploadRelayFileBytes` (`lib/net/BlobClient.h`). The server stores opaque ciphertext in a private bucket.

Signed calls (`POST`, JSON body with `account_id`, `audience`, `timestamp` (epoch ms), `signature`): sign bytes are `domain 0x00 | u8(2) | LenUtf8(audience) | fields | i64be timestamp`, strings `u64be(len) | utf8`, integers `u64be` (`RelayBlobSignPayload`; golden vectors in `relay_sign_vectors.json` and `relay_file_sign_payload_test.cpp`; the account by its Account ID since multi-relay MR001).

| HTTP | Domain | Signed fields | Answer |
|------|--------|---------------|--------|
| `POST /v1/files/start` (`byte_length`) | `pp-browser:relay-file-start-v1` | `account_id`, `byte_length` (u64) | `{ blob_id, mode, part_bytes, part_count, upload_url?, expires_in, pending_expires_at }` |
| `PUT {upload_url}` | — | — | `mode=single`: the ciphertext, exactly `byte_length` bytes (`Content-Length` is signed into the URL; valid 15 min) |
| `POST /v1/files/part` (`blob_id`, `part_number`) | `pp-browser:relay-file-part-v1` | `account_id`, `blob_id`, `part_number` (u64, 1-based) | `{ upload_url, part_bytes, expires_in }`: a PUT URL for that part (valid 1 h); `part_bytes` is **this part's** size (the plan's `part_bytes` except the last, which is the remainder); 400 `INVALID_PART_NUMBER` outside 1..`part_count` |
| `PUT {part upload_url}` | — | — | `mode=multipart`: bytes `(n-1)·part_bytes` … of the ciphertext, exactly that part's size, no `Content-Range`; the relay assembles the parts at `complete` (no ETags kept) |
| `POST /v1/files/complete` (`blob_id`) | `pp-browser:relay-file-complete-v1` | `account_id`, `blob_id` | `{ blob_id, token, share_path, expires_at }`, **once**: a repeat is 400 `BLOB_NOT_PENDING` |
| `POST /v1/files/delete` (`blob_id`) | `pp-browser:relay-file-delete-v1` | `account_id`, `blob_id` | `{ success: true }`; frees the quota, the link stops working |
| `POST /v1/files/list` (`status?` `pending` / `complete`) | `pp-browser:relay-file-list-v1` | `account_id`, `status` (`""` when omitted) | `{ files: [{ blob_id, status, byte_length, created_at, completed_at, expires_at, pending_expires_at, … }], usage: { bytes_used, pending_count, quota_bytes, max_pending } }`, newest first, at most 500, no tokens |
| `GET /v1/files/<blob_id>/download?t=<token>` | unsigned | — | **302** to a 60-second storage URL (`Cache-Control: no-store`); every failure (unknown, expired, used up, deleted) is the same **404** `{"error":"Not found"}` |

`share_path` is `/api/relay/v1/files/<blob_id>/download?t=<token>`; the message's `url` is the relay origin (the relay base URL without `/api/relay`) plus it. **The token is the download credential**: logs, errors and metrics carry the blob id, status codes and `RelayFileUrl::Redacted` (no query) only, and the redirect target is never logged.

Limits (server, fixed): retention 7 days from `complete` (downloads do not extend it), **10 downloads per file** (each 302 counts, finished or not), 1 GiB per account (pending + complete), 20 pending uploads, pending TTL 48 h, single PUT up to 16 MiB, multipart above it with 16 MiB parts, 512 MiB per file. The client sends at most 50 MB per file (`kLargeAttachmentMaxBytes`), so it gets `mode=single` or a multipart plan of up to 3 parts; ciphertext held only in memory (`UploadRelayFileBytes`, at most 4 MiB) treats `multipart` as an error.

Client flow (`UploadRelayFile`): `start(byte_length = ciphertext length)` → `PUT` (streamed from the staged file, `application/octet-stream`) → `complete` → the link is written to the staged record (`OutgoingAttachmentStore`) → the message goes out. The start line logs `mode=`, `parts=` and `part_bytes=`. **Multipart**: parts go one after another, part n from offset `(n-1)·part_bytes` of the start answer (never a size of the client's own), each PUT read from the staged file at that offset (`HttpClient::PutFileRange`); `files/part`'s size must equal the planned one. A part whose `files/part` or PUT gets no answer or a 5xx, or whose PUT gets a 403 (expired URL), is tried again with a fresh `files/part` at most twice; `complete` is tried again at most twice on no answer, a 5xx or `UPLOAD_INCOMPLETE`. A cancel (the progress callback says stop) or any other refusal stops at once; progress is the part's offset plus its bytes sent and never goes back on a retry. No resume across sends (`files/status` is not used). After `start` has answered, **any** failure (the PUT or a part after its tries, `complete`, `BLOB_NOT_PENDING`, an unusable plan, the record write) deletes that `blob_id` (best effort) and reports the original error; the person resends. A `start` refused with `QUOTA_PENDING_COUNT` deletes this account's pending uploads created more than an hour ago (never newer ones: a PUT started in time may still be running, another device may be uploading, clocks drift) and starts once more. Download (`AttachmentFetch`): the only caller with `follow_redirects` (at most 2 hops, https only, public addresses on every hop, only the final body kept and capped); a download that got any answer from the relay, or whose bytes then could not be stored, is suppressed (`AttachmentSuppressionStore`) so automatic re-queues never GET it again, while a failure before any answer (offline, DNS, connect, TLS, timeout) spent nothing and may retry by itself; the card's Download / Retry asks once more.

Failure codes (`{ error, code }`; `ClassifyBlobFailure` takes the `code` first, the log detail is `HTTP <status> <CODE>: <text>`):

| Code (status) | `Err::Blob` | Client does |
|---|---|---|
| `FILE_TOO_LARGE` (400) | `TooLarge` | not retried |
| `QUOTA_FILE_BYTES` (429) | `QuotaExceeded` (`IsRelayFileQuotaError`) | `BlobQuotaRecoveryFlow`: `PlanOldestRelayFileDeletion` lists complete files and picks the oldest by `completed_at` until this file fits (the size the refused start asked for, else the 50 MB send cap plus the AEAD tag), deleting each on `files/delete` (`DeleteRelayFile`) (of the newest 500; if all of them would not free enough, "cloud space is full" and nothing is deleted); after the confirm, deletes them and uploads once more; a second full answer is final |
| `QUOTA_PENDING_COUNT` (429) | `RateLimited` | the stale-pending clean-up above, once |
| `BLOB_NOT_FOUND` (404); a 404 download | `Expired` | "Attachment expired."; a delete of a copy already gone is fine |
| `BLOB_NOT_OWNED` (403) | `UploadRejected` | failure line |
| `UPLOAD_NOT_CONFIGURED` (503) | `UploadNotConfigured` | "server hasn't enabled uploads" (`UploadNotConfiguredNoDirect` after a failed direct push) |
| `BLOB_NOT_PENDING`, `INVALID_BYTE_LENGTH`, `UPLOAD_INCOMPLETE` (400) | `Generic` | failure line (after start: the blob is deleted) |

"Delete for everyone" succeeds once the retract is sent and the message is deleted here; deleting our server copy by the link's blob id (`DeleteUploadedAttachmentCopy`, `files/delete`) is a separate best-effort step whose failure is logged (WARNING, blob id and the delete's own error); an old CDN link has nothing to delete and is skipped (one INFO line). "Delete for me", clearing history and deleting a thread do not touch the server copy (it expires 7 days after `complete`).

## HTTP account lockers (multi-device M023)

Base path under the org's `registration.base_url` (not the preferred relay's: lockers are not moved yet, [Relay services and org services](#relay-services-and-org-services)). Client: `HttpLockerClient` / `ILockerClient`; sealed and checked by `lib/people/AccountLocker` ([M023](../../projects/multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) §4–5). The relay is one locker provider among several: it cannot read a snapshot, and keeps only the newest version.

| HTTP | Purpose |
|------|---------|
| `POST /v1/lockers/put` | `{ account_id, audience, timestamp, signature, locker: { locker_id, version, signing_public_key_b64, sealed_b64, signature_b64 } }` → `200 { locker_id, version }`; **409** when the stored version is the same or newer (also when two writers race); **403** `Locker quota reached` past 4 lockers per account; body ≤ 2 MiB |
| `GET /v1/lockers/:locker_id` | `200 { locker: {…} }` or **404**. By id alone — a restoring device is not registered yet; rate-limited per IP |

Two signatures on a write:

| Half | Signed by | Bytes |
|------|-----------|-------|
| Relay (quota) | the account key, for the org origin's pinned PeerId | `pp-browser:relay-locker-put-v1` ‖ 0 ‖ u8 2 ‖ LenUtf8 audience ‖ LenUtf8 `account_id` ‖ LenUtf8 `locker_id` ‖ u64 `version` ‖ i64 `timestamp` |
| Locker (owner) | the locker ML-DSA-65 key; `locker_id` = lowercase hex of BLAKE2b-256(its public key) | `pp-browser:account-locker-v1` ‖ 0 ‖ u8 1 ‖ LenBytes(locker id) ‖ u64 `version` ‖ LenBytes(sealed) |

The snapshot (`sealed`, the owner signature, rollback protection) and the state inside it: [ACCOUNT_LOCKER.md](ACCOUNT_LOCKER.md). Golden vectors: the owner half in `account_locker_test`, the relay half in `http_locker_client_test` and `relay_sign_vectors.json`. The relay learns which registered user writes which locker (the write is authenticated for its quota); reads carry no identity.

## Client compatibility discovery

Unauthenticated public GET (directory-style; no identity unlock required):

`GET {relay.base_url}/v1/client-compat` — the org's relay (the config's `relay`), whichever relay is preferred ([Relay services and org services](#relay-services-and-org-services))

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

`POST {relay.base_url}/v1/crash-reports` (the org's relay, as client compatibility)  
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

Signed with `pp-browser:relay-api-v1` (sign version 3, the relay's PeerId as audience) ops `DeviceRegister=3` over `(account_id, platform, device_id, push_token)` and `DeviceUnregister=4` over `(account_id, platform, device_id, binding_id)`. Tokens are registered per relay, by Account ID, on every relay of ours that holds our registration ([Several relays](#several-relays)): a relay holding mail for the account wakes its phone. A backend switch unbinds the preferred relay's only; a relay removed from My relays is unbound there ([My relays](#my-relays)).

| HTTP | Purpose |
|------|---------|
| `POST /v1/devices/register` | Bind push token `{ platform, device_id, push_token, account_id, audience, timestamp, signature }` (`platform` is `android` or `ios`) → `{ success, binding_id }` |
| `POST /v1/devices/unregister` | Remove the binding `{ platform, device_id, binding_id, account_id, audience, timestamp, signature }` names (`409 binding_mismatch` for another; the client keeps its binding with alerts off — P005 amended; account revoke removes all of them) |

`ios_voip` bindings (`device_id` `<ios device id>.voip`) are legacy from P016's PushKit token: the app no longer registers them (P019), and the relay should drop them.

On `POST /v1/messages` accept, a conforming relay **best-effort** wakes registered devices for the recipient:

| Condition | Opaque type |
|-----------|-------------|
| Accepted message | `inbox_wake` |
| Accepted message with `"urgent": true`, within the urgent rate limit | `call_wake` |

The relay cannot tell a call invite from a message (the control type is inside the end-to-end payload). The sender says so instead: a call invite's send record carries `"urgent": true` (P012). The hint sits next to the record's fields and is **not** in the signed bytes, so a relay that does not know it ignores it and verifies the request as before. The relay trusts it no further than a rate limit: per sender → recipient, at most one urgent wake per 15 s; over the limit the message wakes as `inbox_wake`. Every wake syncs the inbox, a call invite in it rings, and every ring raises the "Incoming call" notification where the user is not looking (P010 / `CallPresenter`).

| `platform` | Provider shape |
|------------|----------------|
| `android` | FCM **data** message `{ "type": "…" }` |
| `ios` | `inbox_wake`: a visible alert (`apns-push-type: alert`, priority 10, `aps.sound: "msg.caf"`, `aps.alert: { title-loc-key: MESSAGE_INCOMING_TITLE, loc-key: MESSAGE_INCOMING_BODY }`, no sender, chat or content) with custom `type`. `call_wake`: a visible alert (`apns-push-type: alert`, priority 10, `apns-collapse-id: pp-call`, `aps.interruption-level: time-sensitive`, `aps.sound: "call.caf"` (the app bundles that 29 s tone; a build without the file plays the default sound; relay issue #563); `inbox_wake` alerts use `aps.sound: "msg.caf"` (bundled short tone, same fallback) instead of `default`, `aps.alert: { title-loc-key: CALL_INCOMING_TITLE, loc-key: CALL_INCOMING_BODY }` — worded on the device from `packaging/ios/*.lproj`, nothing about the call in the push) with `type: call_wake` (plus `wake_hint` and `sent_at`, below), because iOS throttles silent pushes and does not wake an app the user swiped away. The collapse id is the app's own incoming-call banner id, so the two replace each other. |

A relay may add `sent_at` beside `type` in the `ios` call alert: its own time when it sent the push, Unix **milliseconds**, a JSON number. Unsigned and optional (an older relay omits it). The relay keeps it on its call pushes; the app does not read it.

A send record may also carry `"wake_hint": "<id>"` (<= 64 chars of `[A-Za-z0-9-]`; unsigned like `urgent`): a call invite sends its `call_id`, an ordinary message its envelope `message_id`. A relay that knows it copies it into the APNs payload (the alert) as a top-level custom key `wake_hint`, beside `type`; one that does not ignores it. The app matches a tapped alert to its ring, message or missed-call record by it after the PIN unlock and sync; a push without it opens the first ring as before.

Payloads remain **opaque** — no names, thread ids, or media (an id the sender chose is all `wake_hint` carries). Failures must not fail message store. How the provider authenticates to FCM/APNs is outside this contract. See [projects/push-notifications](../../projects/push-notifications/) and [p2p-av-calls V006](../../projects/p2p-av-calls/DECISIONS.md).

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

A streamed answer refused with `401` (the gateway does not know the Bearer) mints a guest key at the session's own LLM base URL, stores it in `identity.enc` beside the registered key and asks the same question once more with it. Only when that retry is answered is a refused registered key dropped (one read-modify-write); a second `401` leaves the registered key as it was. At most one mint until a turn is answered again; meanwhile a `401` is asked again only when the stored key changed (another turn renewed it), else it is the turn's error, as is a failed mint (B183, `AgentSession`, `BriefLlmKeyRenewal`).

Wire format is OpenAI chat completions, including client tool loops (`assistant.tool_calls` + `role: tool`). www does not adapt messages; the harness maps them to the configured provider.

Lost registered key or expired registration: use **Renew registration** in Me → Profile (finish) to issue a new key. **Rotate Brief API key** remains available while registration is active. Guests remint via `/guest/start` (app caches until expiry / register).

### Streamed answers

`POST https://backend.example.test/api/llm/v1/pp/chat/stream` — JSON request (`message`, optional `intent`, `context.history` / `summary`, `client.capabilities`; up to 6 MB), `text/event-stream` response (`data: {"type": meta|status|token|done|handoff|error}` … `data: [DONE]`, `: ping` heartbeat every 10 s of silence). Same Bearer, limits and error shapes as the completions endpoint; one streamed answer counts as one request. Stopping is closing the connection: www aborts its upstream request and brief_AI stops generating. The wire contract is owned by brief_AI (`docs/contracts/pp-client.md` in that repo, v0.3); www relays the body unchanged and adds no user id. `PP_BROWSER_BRIEF_STREAM_URL` (dev only) redirects the client to a local fake server; the Bearer goes to an override only over https or loopback.

## libp2p (deferred)

Future work adds `Libp2p*Client` implementations behind the same interfaces and sign-byte auth over libp2p HTTP to www.
