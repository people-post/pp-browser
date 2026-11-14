# Multi-relay: design

**Status:** Proposed (2026-11-12), decided in outline ([MR001](DECISIONS.md#mr001--inboxes-are-keyed-by-account-id)–[MR004](DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)); relay identity and audience specified below ([MR007](DECISIONS.md#mr007--relays-have-a-node-style-identity-every-signed-relay-command-names-it-as-its-audience), not built). Pre-release: every change below is a clean cutover ([COMPATIBILITY § No bridges](../../docs/contracts/COMPATIBILITY.md#no-bridges-pre-release)).

## Where it stands

What holds now is in the contracts ([SERVICE_ENDPOINTS § Several relays](../../docs/contracts/SERVICE_ENDPOINTS.md#several-relays), [ACCOUNT_CARD](../../docs/contracts/ACCOUNT_CARD.md#account-card)); phase status is in [CURRENT_STATE.md](CURRENT_STATE.md). In short:

- **Inboxes, sends and polls are addressed by Account ID** on every relay; there are no `relay:` ids. A relay takes a foreign sender's send when its key hashes to its Account ID, and holds strangers to the receiver's price (grants for contacts, MR005).
- **The account card lists the account's relays**, preferred first, and its floor (card version 3). A contact keeps the newest list with its card (`KeptRelays`); a contact added by QR code starts with the locator's preferred origin until the card arrives (`ContactRelays`).
- **An app registers and polls on up to four relays of its own**, the user's list in Me → My relays (seeded from the deployment's relay and `relay.extra_base_urls`), and **sends into the recipient's relays in list order**, with a per-relay health memory and one retry on a stale list. The user's relay services follow the preferred relay; org services stay on the org's origin ([MR006](DECISIONS.md#mr006--the-preferred-relay-carries-the-users-relay-services-org-services-stay-on-the-orgs-origin)).
- **A name search asks every relay of ours**; a stranger's price is its card's floor.
- **The contact page's refresh button** asks the contact's devices, then its relays in list order, then ours, by Account ID, on a worker; keys are kept only when they verify. The page shows the preferred relay by host with whether it answered at the last refresh.
- **The QR locator carries the preferred relay's origin** (optional, within MR004's bounds) and no card.
- **Not yet:** account lockers stay at the org's registration URL; stream history is read from the org's relay while it is ours; the list cannot be empty (no mesh-only choice); only the main device edits it.

## Model

- **A person is their account.** The Account ID is the same on every relay and needs no relay to be checked.
- **Devices are reached over the mesh;** a relay is a mailbox for when they are offline.
- **A relay is named by its origin:** the HTTPS base URL a client uses, e.g. `https://www.brief.global/api/relay`. A relay does not know its own public URL; the client records the one it registered at.
- **Each account publishes its relay list in its account card** ([MR002](DECISIONS.md#mr002--the-relay-list-lives-in-the-account-card)): the relays it is registered on, preferred first. The card is signed by the account key, so any relay or contact can pass it on without being trusted, and the newest card (`issued_at`) wins, as for the device list.

## Account card: `relays`

```json
"relays": [
  { "origin": "https://relay.example.org/api/relay" },
  { "origin": "https://www.brief.global/api/relay" }
]
```

- Ordered: the first entry is the **preferred** relay. An empty list means the account receives over the mesh only.
- *Card version 4 (MR007, not built):* each entry also names the relay's PeerId, `{ "origin", "peer_id" }` — [§ Card version 4](#card-version-4).
- **Bounds** ([MR004](DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)):
  - at most 4 entries;
  - an `origin` is an absolute `https://` URL of at most 128 bytes, with no query or fragment;
  - no duplicates.
- **A card breaking a bound is refused whole**, as for the device list.
- **Covered by the card signature:** a new card sign version. A card without the field is refused, a clean cutover ([ACCOUNT_CARD.md](../../docs/contracts/ACCOUNT_CARD.md)).
- **Size:** a card is about 13 KB plus about 4 KB per device; four relays add at most about 0.6 KB.
- **Where the card goes:** the card exchange over Amp, the contact-card chat message, group join bundles (within `kRosterCardsBudgetBytes`) and relay registration. Each already carries several KB.
- **The QR locator carries no card.** It may add the preferred relay's origin alone (about 40 bytes, [MR004](DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)), so a contact added by QR can reach the user's mailbox before it has the card.

## Relays: inboxes by Account ID

[MR001](DECISIONS.md#mr001--inboxes-are-keyed-by-account-id), [MR003](DECISIONS.md#mr003--cross-relay-delivery-is-an-account-signed-send).

- **Registration:**
  - an account registers on a relay as today: a challenge, signed with the ML-DSA key;
  - the relay keeps one row per Account ID, with the account's keys, its endpoints, and the **account card** it was given;
  - there is no `relay:` id any more: the Account ID is the inbox address;
  - the relay accepts the card only if it verifies and names the same Account ID.
- **Directory:**
  - `GET /users/by-account/{account_id}` returns the row and its stored card, so a stranger learns the account's relays from any relay it is registered on;
  - search by nickname stays per relay;
  - `GET /users/{relay_user_id}` goes away.
- **Sending:**
  - `POST /messages` names `sender_account_id` and `recipient_account_id`;
  - the signed bytes cover both, with a new domain or version;
  - the recipient must be registered on this relay (new check); otherwise the relay answers `recipient_not_registered`, which is not a retryable failure.
- **Who may send:**
  - a sender registered on this relay is checked against its stored key, as now;
  - **a foreign sender** (not registered here) sends its ML-DSA public key with the message. The relay checks that the key hashes to `sender_account_id`, then checks the signature;
  - the relay may cache a verified foreign key by Account ID, so later sends can leave it out.
- **Abuse:** the relay tells contacts from strangers, not "registered here" from "foreign" ([MR005](DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant); below).
- **Receiving:** polling, ack and clear are signed by the recipient's account (as the requester) and address the inbox by Account ID.
- **Push:** devices register their push tokens on every relay the account uses, so any relay holding mail can wake the phone.
- **Not built:** forwarding between relays (federation). A sender posts straight into the recipient's relay.

## Strangers and contacts: the receiver's price

[MR005](DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant), with [pricing P001](../pricing/DECISIONS.md#p001--initiation-floor--media-quote-gates-no-payment-rails-yet).

A relay cannot read a message, and it does not know who a user's contacts are. It needs one signal it can check: whether the sender has a grant from the recipient.

- **The receiver's price is in the account card:** `initiation_floor`, in `pp_credit` minor units, the one price for opening a chat or a call with them (P001). It is signed with the card, so every relay and every sender reads the same price. It replaces the floor sent at directory registration.
- **A contact carries a grant.** When a recipient accepts someone, the recipient's app issues a grant and gives it to the sender over the control lane. "Accepts" means: adds them, accepts their opening offer, or waives it. The grant is signed by the recipient's account key, under domain `pp-browser:relay-grant-v1`, over:
  - `recipient_account_id`;
  - `sender_account_id`;
  - `issued_at`;
  - `expires_at` (30 days, renewed while they stay a contact).

  The sender attaches it to every send into the recipient's relays, and the relay checks it with the recipient's key, which it holds.
  - No contact list is uploaded to any relay. A relay sees only the pairs that send through it, as it does today.
  - Blocking stops renewal. Until the grant expires, the recipient's app drops that sender's mail.
- **A relay treats a send without a grant as a stranger's,** whether the sender is registered here or not:
  - **floor above 0:** the send must carry an opening offer of at least the floor. Until payment rails exist, no offer can be honoured, so the relay refuses it (`payment_required`). That matches P001's outbound block, and spares the recipient the spam altogether.
  - **floor 0:** the send is accepted within a first-contact quota per recipient (a few per stranger, a bounded number per hour for all strangers together). It raises no message alert; a call wake is allowed within the per-pair limit.
- **Safety limits apply to everyone:**
  - per sender account;
  - per client address for senders the relay has not verified before (configurable: carrier-grade NAT puts many phones behind one address);
  - the mailbox caps.

## App

- **My relays** (Me → Network):
  - the list, preferred first, kept in the profile (seeded once from the deployment's relay and `relay.extra_base_urls`);
  - add a relay by origin (it must answer), remove one (not the preferred), mark one preferred;
  - each with its registration (kept across restarts) and reachability (its last poll);
  - only the main device changes it; a linked device follows the card's list.
  - Changing the list signs a new card, registers on new relays, publishes the card to every relay in the list and to contacts over the control lane, and revokes the registration (with its push bindings) and drops the inbox cursor on removed relays.
- **Relay services and org services** ([MR006](DECISIONS.md#mr006--the-preferred-relay-carries-the-users-relay-services-org-services-stay-on-the-orgs-origin)):
  - the preferred relay carries the inbox, push, registration, the directory (first), profile photos and blobs, chat attachment files;
  - the org's origin keeps the Brief LLM gateway and its key, Support, diagnostics, crash reports, client compatibility;
  - account lockers and stream history stay on the deployment's relay (no migration yet).
- **Registering and receiving:**
  - the app registers on each listed relay;
  - it polls the preferred relay at today's pace and the others more slowly (e.g. a quarter of that);
  - it drops duplicates by message id (delivery already dedupes).
- **Sending:**
  1. over the mesh when a device of the recipient is online (as today);
  2. else to the recipient's relays in list order: the preferred first, the next on a transport failure or a 5xx.
  - `recipient_not_registered` means our copy of their list is stale: fetch the card and try again once.
  - A per-relay health memory skips a relay that just failed, as the circuit client does for relays.
- **Contacts:**
  - a contact stores the relay list from its newest verified card (a ladder step in `profile.db`);
  - a contact added by QR starts with the locator's preferred origin, if any.
- **The refresh button** asks, in order of trust:
  1. the contact's devices over the mesh (the card exchange);
  2. the contact's relays in list order, by Account ID;
  3. our own relays.
  - The relays are asked up to the first whose answer for the account verifies (a relay down, one without the account, or one whose answer does not verify gives way to the next); one asked as the contact's is not asked again as ours.
  - An answer is used only when it verifies against the Account ID (the relay's stored card and its row's keys alike); an unverified one contributes nothing (no keys, name, photo or addresses) and the page shows that relay as not reachable. Name and photo come from the newest verified answer: the newest kept card's name when a device or a relay's card brought one, else the relay row's; the photo is the row's.
  - It runs on a worker, shows for every contact, says what changed, or that nothing could be reached or found (the plain *card* wording; no source named), and shows the preferred relay with its reachability: what that relay answered at the last refresh (in memory; "not known yet" before one asked it), so no probe runs on its own.
- **Our own sends:** the sender signs with its account key. A send to a relay we are registered on goes as a registered sender; to any other relay, as a foreign sender with its key.
- **Grants:** the app issues one to each accepted contact, renews it before it expires, keeps the grants it was given, attaches the matching one to each send, and stops renewing a blocked contact's. A send that gets `payment_required` shows the receiver's price (P001's pay-or-alternatives UI).

## Relay identity and audience (MR007)

[MR007](DECISIONS.md#mr007--relays-have-a-node-style-identity-every-signed-relay-command-names-it-as-its-audience). **Accepted, not built.** This section is the implementers' reference (relay: pp-relay-apis, TypeScript; app and pp-node: C++) until it ships and is promoted to `docs/contracts/` (SERVICE_ENDPOINTS, ACCOUNT_CARD, DATA_LAYOUT, COMPATIBILITY). Pre-release clean cutover: no old sign version, card version or body shape is accepted after it ([COMPATIBILITY § No bridges](../../docs/contracts/COMPATIBILITY.md#no-bridges-pre-release)).

**Threat:** a relay on a user's list (honest-but-curious or hostile) replaying the user's signed calls at the user's other relays, or at any relay the user's contacts use. Not a network attacker: TLS covers that. Not the relay misusing its own copy: a relay can already do anything to its own data.

### The relay's identity

**Key.** Each relay deployment (`qa`, `prod`, each lab stack) has its own ML-DSA-65 key pair. The secret is the 32-byte FIPS 204 key-generation seed (ξ), stored as plain text base64 in the relay's Secrets Manager secret. Its PeerId is `pp::PeerIdFromMlDsaPublicKey(public key)` ([MESH_IDENTITY](../../docs/architecture/MESH_IDENTITY.md#derivation-pipeline-frozen): `08 04 12 A0 0F ‖ key` → `12 20 ‖ SHA-256` → base58btc, `Qm…`); pp-relay-apis' `peerIdFromMlDsaPublicKey` already matches it.

| Variable | Functions | Value |
|---|---|---|
| `RELAY_IDENTITY_PUBLIC_KEY` | registry, messenger, storage, push, support-worker (every function that verifies or makes a signed relay call) | standard base64 of the raw 1952-byte public key (public, a plain variable); each derives the PeerId once per instance |
| `RELAY_IDENTITY_KEY_SECRET_ID` | registry only | Secrets Manager id of the seed (`secretsmanager:GetSecretValue` on it, registry only; cached 15 min per instance like the LLM token key) |

- The registry checks once per instance that the secret's seed generates `RELAY_IDENTITY_PUBLIC_KEY`. Nothing else uses the private key in this step; it exists now so that the identity is real when sealing (MR007 item 6) or Amp needs it, and so no function but the registry can read it.
- **Missing or inconsistent configuration fails closed:** a function without a valid `RELAY_IDENTITY_PUBLIC_KEY` answers every signed call, and the registry `GET /v1/relay-identity` too, with `503 relay_identity_unavailable` (and the config check reports the missing variable as the others do, `ConfigError`). A relay never accepts a call without checking its audience.
- **Key generation:** a script in pp-relay-apis (`scripts/relay-identity-keygen.mjs`, `@noble/post-quantum`'s `ml_dsa65.keygen(seed)` from 32 random bytes) prints the seed (for the secret) and the public key and PeerId (for the variable and the infra repo's record). Local and lab stacks run the same script at setup and give each stack its own key through the same secret mechanism the local stack uses for the LLM token signing key; two lab stacks must never share a key.
- **Rotation is a new relay.** A new key is a new PeerId: every app shows that relay as *identity changed* and stops using it until its user accepts the new one; every contact's card that names the old PeerId is skipped by senders until its owner accepts and re-issues; every pp-node stops its relay calls until its pin is cleared. So a relay's key is rotated only after a compromise, and before sealing ships nothing checks a signature by it, so a leaked seed alone harms no one yet.

**Endpoint.** `GET {base}/v1/relay-identity` (base = the relay base URL, `<origin>/api/relay`), served by the registry beside `client-compat`, unauthenticated, rate-limited per client address like the other unsigned GETs.

```json
{
  "schema_version": 1,
  "peer_id": "Qm…",
  "public_key_b64": "…",
  "signature_alg": "ml-dsa-65"
}
```

| Field | Rule |
|---|---|
| `schema_version` | `1`; a client refuses a newer one |
| `peer_id` | the PeerId, base58btc as nodes print it |
| `public_key_b64` | standard (padded) base64 of the raw 1952-byte ML-DSA-65 key, the same encoding as `signing_public_key_b64` on cards |
| `signature_alg` | `ml-dsa-65` (the only one) |

- `200` with `Cache-Control: public, max-age=300` (overrides the router's `no-store`; the CDN does not cache `/api/relay/*` anyway, and a stale copy only delays noticing a rotation). `503 relay_identity_unavailable` as above. Unknown keys are ignored.
- **The client accepts it only when** `schema_version` is 1, `signature_alg` is `ml-dsa-65`, the key decodes (canonical base64) to 1952 bytes and `PeerIdFromMlDsaPublicKey(key) == peer_id`; anything else is `relay_identity_invalid`, handled as no identity.
- **Not signed, on purpose.** A static self-signature is copied by any relay that serves it; a signature over a client nonce is obtained by a relay that forwards the nonce to the real one. Neither stops a relay from claiming another's PeerId. That claim is made harmless by the pin's uniqueness ([§ Pins](#pins-origin--peerid)), and possession of the key is proved once there is something to prove it for (sealing, or the Amp handshake).
- The OpenAPI contract (`openapi/relay.yaml`) and `SPECS.md` gain the path in the relay PR.

### Sign bytes: the audience

**Rule, every command domain:** `domain ‖ 0x00 ‖ u8 version ‖ LenUtf8(audience) ‖ …the domain's fields as before…`, where `audience` is the relay's PeerId as the base58btc string (46 bytes for an ML-DSA key) and `LenUtf8` is `u64be(len) ‖ utf8`, as every string in these layouts. In pp-relay-apis this is one change to `versioned(domain)` (it takes the audience and writes it after the version) and to `relayApiHeader(op, timestamp)` (audience after the version, before the op); registration's builder writes it the same way.

*Why the string and not the 34-byte multihash:* every field here is already a LenUtf8 string (Account IDs, the card's device `peer_id`s); base58btc of a 34-byte multihash that starts with `0x12` has exactly one spelling, so the string is canonical; the relay compares it byte for byte with its own before building any bytes, so no second spelling can be verified; and the relay needs no base58 decoder on the hot path. Twelve bytes more per call do not matter.

**Body.** Every signed call's JSON body (and `register/finish`'s) gains a top-level `"audience": "<PeerId>"` beside `timestamp` and `signature`. It is what the relay checks; the bytes it verifies are built with its own PeerId.

| Domain | Old version | New version | Fields after `LenUtf8(audience)`, in order |
|---|---|---|---|
| `pp-browser:relay-register-v1` | 2 | **3** | `challenge` LenUtf8, account ML-DSA-65 key (1952 raw), account ML-KEM-768 key (1184 raw), `signature_alg` u8 (`1`), `timestamp` i64 |
| `pp-browser:relay-revoke-v1` | 1 | **2** | `account_id`, **`registration_id`**, `timestamp` |
| `pp-browser:relay-profile-v1` | 1 | **2** | `account_id`, `nickname`, `timestamp` |
| `pp-browser:relay-profile-icon-v1` | 1 | **2** | `account_id`, `url`, `blob_id`, `kind`, `timestamp` |
| `pp-browser:relay-heartbeat-v1` | 1 | **2** | `account_id`, `peer_id`, `state`, `timestamp` |
| `pp-browser:relay-llm-token-v1` | 1 | **2** | `account_id`, `timestamp` |
| `pp-browser:relay-locker-put-v1` | 1 | **2** | `account_id`, `locker_id`, `version` u64, `timestamp` |
| `pp-browser:relay-blob-presign-v1` | 1 | **2** | `account_id`, `content_type`, `byte_length` u64, `purpose`, `timestamp` |
| `pp-browser:relay-blob-retain-v1` | 1 | **2** | `account_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-blob-delete-v1` | 1 | **2** | `account_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-blob-list-v1` | 1 | **2** | `account_id`, `status_filter`, `timestamp` |
| `pp-browser:relay-file-start-v1` | 1 | **2** | `account_id`, `byte_length` u64, `timestamp` |
| `pp-browser:relay-file-part-v1` | 1 | **2** | `account_id`, `blob_id`, `part_number` u64, `timestamp` |
| `pp-browser:relay-file-status-v1` | 1 | **2** | `account_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-file-complete-v1` | 1 | **2** | `account_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-file-delete-v1` | 1 | **2** | `account_id`, `blob_id`, `timestamp` |
| `pp-browser:relay-file-list-v1` | 1 | **2** | `account_id`, `status_filter`, `timestamp` |
| `pp-browser:relay-api-v1` | 2 | **3** | `op` u8, `timestamp` i64, then the op's fields (below) |

Strings are LenUtf8, integers big-endian; all field rules (trimming, `""` for absent) are unchanged. Domain names keep their `-v1` suffix (the u8 is the version, as `relay-register-v1` is already at 2).

`relay-api-v1` version 3, the op's fields after `op` and `timestamp`:

| Op | Fields |
|---|---|
| send (0) | `sender_account_id`, `recipient_account_id`, `stream_id`, `index_key` u64 |
| poll_inbox (1) | `requester_account_id`, `cursor` |
| stream_history (2) | `requester_account_id`, `sender_account_id`, `stream_id`, flags u8 (bit 0 min, bit 1 max), [`min_index_key` u64], [`max_index_key` u64], `limit` u64, `order` u8 |
| device_register (3) | `account_id`, `platform`, `device_id`, `push_token` |
| device_unregister (4) | `account_id`, `platform`, `device_id`, **`binding_id`** (replaces `push_token`, which the relay never used for removal) |
| ack_inbox (5) | `requester_account_id`, `cursor` |
| clear_inbox (6) | `requester_account_id`, `before_created_at` |

**Not commands, no audience** (unchanged by this step except the card's relay entries):

| Signature | Why it carries no audience |
|---|---|
| `pp-browser:kem-binding-v1` | A standing statement that a KEM key is the account's; every relay and peer checks the same one |
| `pp-browser:account-card-v4` | The account's word about itself, carried by relays, contacts and groups; it must verify everywhere (its version changes for `peer_id`, below) |
| `pp-browser:relay-grant-v1` | The recipient's statement to a sender, checked by **each** of the recipient's relays; an audience would make it good at one |
| `pp-browser:account-locker-v1` (owner half) | The snapshot's own signature, checked by a restoring device from any provider. The quota half (`relay-locker-put-v1`) is a command and gets the audience |
| Envelopes, posts, link bundles, control-lane records | End to end inside blobs or over Amp; no relay acts on them |

### The verifier

pp-relay-apis' `createSignedRequestVerifier` (every registered-key call) and `registerFinish`:

1. Parse the JSON body.
2. **`audience`** — absent, not a string, or not byte-equal to this relay's PeerId → `401 { "error": "Signed for another relay", "code": "wrong_audience" }`. Checked first: before the timestamp window, the account lookup, the signature, and (for `register/finish`) before the challenge is consumed, so a misdirected finish does not spend the challenge.
3. Everything else as today (fields, window, lookup, expiry, signature), the sign bytes built with this relay's PeerId.

An old client (no `audience` in the body) gets `wrong_audience`, never a confusing signature failure. A body whose `audience` was rewritten to this relay's PeerId fails the signature (`401 Signature verification failed`).

**What the client does on `401 wrong_audience`:**

| Where | The client |
|---|---|
| A relay of ours, or the org's origin (lockers, LLM token) | Fetches the identity once (at most once per origin per 5 min). Another PeerId than the pin → the relay's state becomes *identity changed* ([§ Pins](#pins-origin--peerid)); no signed call goes there until the user accepts. The same PeerId as the pin → a misbehaving relay: the call fails as a 5xx would (health memory, retried later), logged |
| A contact's relay (a send) | That relay is skipped for this send and the next of the recipient's list is tried; it counts like `recipient_not_registered` toward the stale-list fetch ([SERVICE_ENDPOINTS § Sending by the list](../../docs/contracts/SERVICE_ENDPOINTS.md#sending-by-the-list) step 4: the contact may have re-issued its card for the relay's new identity); it does not mark the relay unhealthy. When the newest card still names the refused PeerId, that relay is skipped for this recipient for 5 minutes |
| pp-node | Fetches the identity; another PeerId than its pin → logs an ERROR (`relay identity changed: pinned <X>, presented <Y>`) and stops its relay calls (registration, heartbeat) until the operator clears the pin or sets the configured one; the mesh services keep running |

### Pins: origin ↔ PeerId

The HTTPS stand-in for an Amp handshake: the app signs for a PeerId at one origin only.

**File:** `relay_pins.json` in the profile directory (per profile, so per backend; plaintext like `our_relays.json` and `contacts.json`, written atomically):

```json
{
  "schema_version": 1,
  "pins": [
    { "origin": "https://www.brief.global/api/relay", "peer_id": "Qm…", "pinned_at_ms": 1761235200000,
      "presented_peer_id": "Qm…", "presented_at_ms": 1761235300000 }
  ]
}
```

- `origin` — the relay base URL, normalized as the card compares relays (scheme and host lower case, `:443` as no port). One pin per origin, **one origin per PeerId**.
- `presented_peer_id` / `presented_at_ms` — optional: another PeerId the origin presented after a `wrong_audience`; its presence is the *identity changed* state.
- At most 32 pins. When full, the oldest pin that is neither one of our relays, nor the org's origin, nor a kept contact's locator relay goes (its next use pins again by fetch).
- Another `schema_version`, or a file that is not JSON, is refused and treated as empty (pins are fetched again: TOFU once more), with an error logged.

**Where a pin comes from — only from the origin's own identity answer** (or, on a linked device, from our own card):

| When | Pinned |
|---|---|
| First start after the upgrade | each relay of ours and the org's origin (`relay` / `registration` base URLs), before anything is signed for it |
| Me → My relays → **Add** | the new relay, after its `client-compat` answer |
| A send or a card fetch for a contact with only a locator relay (no card yet) | that origin |
| A linked device takes a newer card of ours | each of its `{origin, peer_id}` entries (our own account's word), replacing a pin that disagrees and dropping another origin's pin of the same PeerId |

A card from anyone else never writes a pin: a contact could otherwise list a stranger's origin with our relay's PeerId and lock us out of our own relay. A fetch that fails (unreachable, `503`, invalid) pins nothing; nothing is signed for that origin until a later fetch succeeds (it is retried on the registration backoff, 1 min doubling to 30 min).

**Conflicts:**

| Case | Result |
|---|---|
| The origin presents the PeerId it is pinned to | used |
| The origin presents another PeerId than its pin | *identity changed*: no signed call goes there; pins and cards keep the old PeerId until the user accepts |
| The origin presents a PeerId pinned to another origin | refused: `relay_identity_taken` (Add: "This relay is already on your list as {host}" / "already known as {host}"); never pinned |
| A contact's card names `{origin, P}` and our pin for that origin is another PeerId, or P is pinned to another origin | that relay is skipped for sends to that contact (contact page: *identity doesn't match*); the card is still kept, the conflict is logged once per card |
| A contact's card names `{origin, P}` and we hold no pin for either | P is used as the audience as is (no pin written); the relay itself refuses a wrong P (`wrong_audience`) |

**What the user sees** (copy per [UI_DESIGN_SYSTEM](../../docs/ui/UI_DESIGN_SYSTEM.md); confirm-leading actions end with `…`):

- **Me → My relays**, a relay whose identity changed: its row reads *Identity changed — not used* and offers **Accept new identity…** beside Remove…. The confirm says to accept only if the relay's operator announced a change. Accepting re-pins it to the presented PeerId (clearing `presented_*`), registers there, binds push, re-issues our card with the new PeerId and announces it to contacts. While changed: it is not polled, not registered on, not sent to, and it is **left off our card** (the card is re-issued without it at once). Remove… on such a relay revokes nothing (we never sign for an unaccepted identity) and only drops it from the list; its old registration expires there.
- A changed **preferred** relay: its relay services stop (inbox, push, photos, files, the identity's registration) until accepted or another relay is made preferred; the other relays keep receiving.
- The **org's origin**, when it is not on My relays and its identity changed: one line on My relays, *Your app's service server changed identity*, with the same **Accept new identity…**; until then locker writes and LLM token renewals stop (logged).
- **Add** refuses with `relay_identity_unavailable` ("This server doesn't identify itself as a relay"), `relay_identity_taken` (above), or, for an origin pinned before with another PeerId and not on our list (a contact's relay we saw), asks *This relay shows a different identity than when it was last seen. Add it anyway…* and re-pins on confirm.
- **Contact page:** the preferred relay's reach line gains *identity doesn't match* (`ContactRelayView::Reach`), from the last refresh or send.

### Where each client takes the audience

| Client | Audience |
|---|---|
| The app, its own relays (register, poll, ack, clear, history, push, profile, icon, blobs, files) | the pin for that relay's origin, fetched and pinned **before** the first signed call (so before `register/finish`) |
| The app, the org's origin (lockers at `registration.base_url`, the LLM token renewal at `/api/llm/v1/token/renew` — the app does not call it yet; when it does) | the pin for the org's origin (pinned whether or not it is on My relays) |
| The app, a contact's relay (a send) | the `peer_id` of that entry in the contact's newest card (version 4), subject to the conflicts above; no card but a locator relay → the pin by fetch |
| The app, our own other devices' copy | our relays' pins, as our own relays |
| pp-node (register, heartbeat, revoke) | its relay's PeerId: the node config's `registration.relay_peer_id` when set (the operator's pin; the fetch must agree, else the node stops its relay calls as above), else fetched at start and pinned in the node's data directory (`relay_pins.json`, same format) |
| pp-relay-apis support worker (register, poll, send, as its own client) | its own relay's PeerId, derived from `RELAY_IDENTITY_PUBLIC_KEY` (same deployment; no fetch); its card (v4) lists its relay as `{origin from RELAY_API_BASE, that PeerId}` |

### Card version 4

```json
"card_version": 4,
"relays": [
  { "origin": "https://relay.example.org/api/relay", "peer_id": "Qm…" },
  { "origin": "https://www.brief.global/api/relay", "peer_id": "Qm…" }
]
```

Everything else in the card is version 3's ([ACCOUNT_CARD § Account card](../../docs/contracts/ACCOUNT_CARD.md#account-card)).

- **Bounds** (MR004 style, refused whole by app and relay alike, `invalid_card_relays` at the relay): at most 4 entries; `origin` as in version 3; `peer_id` required, a string that base58btc-decodes to exactly 34 bytes `0x12 0x20 ‖ 32 bytes` (every ML-DSA PeerId's form, 46 characters, `Qm…`) and re-encodes to the same string; no two entries naming the same relay by origin (version 3's comparison) **or by `peer_id`**. The card cannot check that a relay's key derives its PeerId (it carries no relay key); the relay itself does (`wrong_audience`).
- **Sign bytes:** domain `pp-browser:account-card-v4` + NUL, u8 `4`, the raw account ML-DSA-65 key, the raw ML-KEM-768 key, `nickname` LenUtf8, device count u64, each device in `peer_id` byte order (`peer_id` LenUtf8, raw device ML-DSA-65 key, raw device ML-KEM-768 key), relay count u64, **each relay in list order: `origin` LenUtf8, `peer_id` LenUtf8**, `initiation_floor` i64, `issued_at` i64.
- **Refused:** every other `card_version` (3 included), everywhere: registration (`invalid_account_card`), directory answers, the card exchange, envelopes, group bundles, contact-card messages. The relay serves only a stored version 4 card from `GET /users/by-account` (a stored version 3 one is left out of the answer; the floor copied from it on `AUTH` stays until the account registers a version 4 card).
- **Ours:** lists our `https://` relays that have a pin in force (not *identity changed*), in My relays order, each with its pinned PeerId; a relay not pinned yet is left off and the card re-issued once it is. Issued at the first start after the upgrade (once our relays are pinned) and announced to contacts and siblings over the control lane (`AnnounceOwnDevicesToContacts`, the device-list notice), so contacts can send by it.
- **Size:** about 55 bytes more per relay; four relays add at most about 0.8 KB to the card.
- **A contact keeps** the newest version 4 card's list as `[{origin, peer_id}]` (`KeptRelays`). `profile.db` 7 → 8 (the step 6 → 7 pattern): our own kept version 3 card becomes the version 4 shape without relays and with its old signature, so it does not verify and is re-issued with the same device list; anyone else's is dropped to its key card and asked for again (the next envelope's `sender_card_at`, or the contact page's refresh). `contacts.json` `remote.locator_relay` stays an origin (the locator does not change).
- **Until a contact's version 4 card arrives**, sends to it go by the rules for a recipient without a card ([SERVICE_ENDPOINTS § Sending by the list](../../docs/contracts/SERVICE_ENDPOINTS.md#sending-by-the-list): a locator relay, else our preferred relay), and `recipient_not_registered` fetches its newest card as today.

### Commands repeated late: the audit

The question for each: what happens when **our own** app's command arrives twice, or late (a retry, a request held in the network up to the ±5 min window), after the app has moved on. Replays by other relays are the audience's job.

| Command | Repeated or late | Verdict |
|---|---|---|
| `register/finish` | the challenge is single-use: a repeat is refused | safe. Residual: a finish held until after we revoked (within the challenge TTL) registers us again; the app sends a revoke only after its in-flight finish has answered or timed out (`RelayRegistrationsWorkflow::Revoke` already waits for a run), and a stray registration expires |
| **`register/revoke`** | **kills the registration made after it** (remove a relay, add it back; a sibling re-registered) | **unsafe → scoped by `registration_id`** |
| `profile/nickname`, `profile/icon` | sets a value: the same value twice is one; a late older one can put back an older name or icon within the window (an older hosted icon whose blob is gone is refused) | idempotent, unchanged; the ordering residual is accepted (the next registration or card carries the current name) |
| `mesh/heartbeat` | the relay refuses a timestamp not newer than the last (`409 stale_timestamp`) | safe, unchanged |
| `llm/token/renew` | strictly increasing timestamp; a repeat is refused | safe, unchanged |
| `lockers/put` | versioned: the same or an older version is `409` | safe, unchanged |
| `blobs/presign`, `files/start` | a repeat makes another pending object | safe (an orphan pending upload, cleaned after 48 h, within the 20-pending cap), unchanged |
| `blobs/retain`, `files/part` | idempotent | safe, unchanged |
| `blobs/delete`, `files/delete` | blob ids are never reused: a late delete deletes what it named | safe, unchanged |
| `files/complete` | once; a repeat is `400 BLOB_NOT_PENDING` | safe, unchanged |
| `blobs/list`, `files/list`, `files/status`, `inbox/poll`, `streams/messages/query` | reads | safe, unchanged |
| `messages` (send) | dedup by (recipient, sender, stream, index key): a repeat is a success that stores nothing | safe, unchanged |
| `inbox/ack` | soft (validates only) | safe, unchanged |
| `inbox/clear` | deletes rows older than an absolute `before_created_at`; the app clears only rows at least a day old, so a repeat finds nothing new to delete | safe, unchanged |
| `devices/register` | upsert per device; a late one after an unregister binds again | idempotent; residual accepted: the app unregisters only on a backend switch (the old backend's binding wakes nothing we poll) or a relay leaving (the revoke after it drops every binding) |
| **`devices/unregister`** | **removes the binding made after it** (by `device_id`, whatever the token) | **unsafe → scoped by `binding_id`** |

**`registration_id`** (relay):
- `register/finish` answers `registration_id`: an opaque string of 16–64 `[A-Za-z0-9_-]`, new for a fresh registration and the same across renewals of it. Suggested: base64url of SHA-256(`"pp-relay:registration-id" ‖ 0x00 ‖ internal relay user id`) cut to 22 characters, so existing rows need no migration and the internal id stays off the wire.
- `register/revoke` body `{ account_id, registration_id, audience, timestamp, signature }`. The relay reads the registration consistently (not the 60 s `AUTH` cache) and removes it in one conditional write on that registration's id; another id → `409 { "code": "registration_mismatch" }`, nothing changed. Purge of push devices only after a removal.
- The app keeps `registration_id` with each registration: the identity's (preferred relay) and each other relay's record in `our_relays.json` (`registration.registration_id`). A revoke with no id kept, or answered `registration_mismatch`, renews first (`register/start` + `finish`, which answers the current id) and revokes with it, once.

**`binding_id`** (relay):
- `devices/register` answers `{ success: true, binding_id }`: a new opaque id (same alphabet) on every call, stored on the device row.
- `devices/unregister` signs and sends `binding_id` (op 4 above) and removes the row only when it matches (a conditional delete); another id, or no row → `409 { "code": "binding_mismatch" }` for another id, `200` for no row (nothing to remove).
- The app keeps the newest `binding_id` per relay in memory (it registers at every start); an unregister with none, or answered `binding_mismatch`, registers first and unregisters with its answer, once.

**Self-heal: `not_registered`.** Today only pp-node re-registers when its relay says it is not registered (`NodeHeartbeat::HandleNotRegistered`); the app does nothing. After MR007:
- Any signed call to a relay of ours answered `403 not_registered` / `registration_expired` while we hold a registration there that we believe current (registered, not expired, not leaving) marks it lost and makes it due now: another relay's through `RelayRegistrationsWorkflow`; the preferred relay's is the identity's, and it registers again whatever the auto-renew preference (the person already registered there). Backoff 1 min doubling to 30 min; on success push binds again and polling resumes.
- Not for a contact's relay (a send keeps its recipient semantics), not for a relay *leaving* or *identity changed*.
- Residual: a linked device that has not yet taken the card removing a relay may re-register there once; the stray registration expires.

### Golden vectors

pp-browser generates one file and both sides test against it; the relay copies it byte for byte.

- **pp-browser:** `capabilities/delivery/src/lib/delivery/tests/data/relay_sign_vectors.json`, checked by `relay_sign_vectors_test.cpp` (delivery sits above net and people, whose builders it needs); `PP_WRITE_RELAY_SIGN_VECTORS=<path>` writes a fresh one (the pattern of `post_vectors_test.cpp`: keys from fixed seeds, signatures randomized). Builders the app does not call yet (`file-status`, `llm-token`, the blob list / retain / delete) are added so every domain the relay verifies is covered.
- **pp-relay-apis:** `packages/shared/src/sign-payload/relay-sign-vectors.json`, read by `relay-sign-vectors.test.ts` (builders, the parser and the verifier) and by `account-card.test.ts` (the card). A change to either side's bytes changes the file in pp-browser first.

```json
{
  "format": "pp-browser relay sign vectors v1",
  "now_ms": 1761235200000,
  "relays": {
    "a": { "seed_hex": "07…(32 bytes)", "public_key_b64": "…", "peer_id": "Qm…" },
    "b": { "seed_hex": "08…(32 bytes)", "public_key_b64": "…", "peer_id": "Qm…" }
  },
  "peer_id_fixtures": [
    { "public_key_fill_byte": "42", "peer_id": "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP" },
    { "public_key_fill_byte": "01", "peer_id": "QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie" }
  ],
  "account": { "public_key_b64": "…", "kem_public_key_b64": "…", "kem_binding_signature_b64": "…", "account_id": "account:…" },
  "calls": [
    {
      "name": "relay-api.poll_inbox",
      "domain": "pp-browser:relay-api-v1", "version": 3, "op": 1,
      "method": "POST", "path": "/v1/inbox/poll",
      "body": { "requester_account_id": "account:…", "cursor": "", "audience": "<relays.a.peer_id>", "timestamp": 1761235200000, "signature": "…" },
      "sign_bytes_hex": "…",
      "expect": { "a": "ok", "b": "wrong_audience" }
    }
  ],
  "account_card": { "card": { "card_version": 4, "…": "…" }, "sign_bytes_len": 0, "sign_bytes_sha256_hex": "…" },
  "refused_cards": [ { "name": "version_3", "card": { "…": "…" } }, { "name": "relay_without_peer_id", "card": { "…": "…" } }, { "name": "two_relays_one_peer_id", "card": { "…": "…" } } ]
}
```

- `relays.*.seed_hex` records how pp-browser made the key (`MlDsa::GenerateKeyPairFromSeed`); the relay uses only `public_key_b64` (as its `RELAY_IDENTITY_PUBLIC_KEY`) and checks `peer_id` against its own derivation, with the two MESH_IDENTITY fixtures.
- `calls` covers **every domain and every op** in the version table, each signed by `account` for relay `a`, and three more: `relay-api.poll_inbox.no_audience` (the body without `audience`: `wrong_audience` at both), `relay-api.poll_inbox.audience_swapped` (`audience` rewritten to `b`'s PeerId, the signature still for `a`: `{"b": "bad_signature"}`), and `register.finish` (challenge `"golden-challenge"`; the relay checks its bytes and signature with its builder, its handler tests use their own challenges). `expect` values: `ok`, `wrong_audience`, `bad_signature`; the relay's test freezes its clock at `now_ms` and registers `account` first.
- Sign bytes are compared as hex; signatures are only verified (ML-DSA signing is randomized), never compared.

### Cutover

1. **pp-relay-apis PR** (Leo reviews): identity config and endpoint, audience in every builder and the verifier, `registration_id` / `binding_id`, card version 4 (version 3 refused, stored ones not served), the support worker, the keygen script, `openapi/relay.yaml` + `SPECS.md`, the vectors copied from the pp-browser branch.
2. **Infra** (Leo, `pp-relay-infra-terraform`): per environment (`qa`, `prod`, different keys) a secret with the seed, `RELAY_IDENTITY_PUBLIC_KEY` on registry, messenger, storage, push and support-worker, `RELAY_IDENTITY_KEY_SECRET_ID` and `GetSecretValue` on the registry. Before the relay deploys: without them every signed call is `503`.
3. **Relay on `qa`** (merge to `develop`). From here every app and pp-node on `qa` built before MR007 is refused on every signed call (`wrong_audience`; old cards `invalid_account_card`): no registration, mail through the qa relay, push or heartbeat (the mesh, search, lookups and `client-compat` still work). Dogfood on qa uses the R5 branch builds.
4. **App and pp-node** (pp-browser PR, R5): built and tested against qa, the app lab green ([below](#tests)).
5. **Prod together, one window:** the app build available to testers (Hongwei triggers TestFlight / the stores), then the relay's release PR to `main` (prod) with `client-compat`'s `min_client_version` raised to the R5 build in the same deploy (old apps show the blocking update instead of failing silently), then the pp-node fleet (`pp-relay-infra-ansible`, Leo) at once: an old node's heartbeats are refused, so it goes stale and drops out of `GET /v1/mesh/nodes` after the stale threshold. Between the prod deploy and a user's update, that user's relays refuse its calls and its mail waits on the relay (nothing is lost; sends to it still store).

### Tests

Design sentences each test guards (cite the ID in the test's comment):

| ID | Sentence |
|---|---|
| MR007-ID | A relay serves `{peer_id, public_key_b64, signature_alg}`, the PeerId derived from its key as a node's; a misconfigured relay answers `503 relay_identity_unavailable` and accepts no signed call |
| MR007-AUD | A signed command verifies only at the relay whose PeerId it names; elsewhere `401 wrong_audience`, checked first; a rewritten `audience` fails the signature |
| MR007-VEC | Both sides build the same bytes for every domain and op (the vector file) |
| MR007-PIN | A pin comes only from an origin's own identity answer (or our own card on a linked device); one PeerId per origin and one origin per PeerId; a changed identity gets no signed call until accepted |
| MR007-CARD | Card version 4 names each relay `{origin, peer_id}` within the bounds; version 3 is refused everywhere |
| MR007-SEND | A send into a contact's relay names the PeerId the contact's card signed; a pin conflict or `wrong_audience` skips that relay, without marking it unhealthy |
| MR007-SCOPE | A revoke or an unregister acts only on the registration or binding it names; a stale one changes nothing |
| MR007-HEAL | `not_registered` from a relay of ours where we believe we are registered re-registers, with backoff |
| B-RELAY-AUDIENCE | A relay refuses signed calls that our app made for another relay (lab) |

**pp-relay-apis:** `relay-sign-vectors.test.ts` (MR007-VEC, MR007-AUD: every `calls` entry against relays `a` and `b`); identity endpoint and config (MR007-ID: the vector's key gives its PeerId; missing variable, secret not matching the public key → 503 on the endpoint and on a signed call); `registerFinish` refuses `wrong_audience` before consuming the challenge (MR007-AUD); revoke (MR007-SCOPE: `registration_id` stable across renewal and new after revoke and register; a revoke with the old id after re-registration answers 409 and leaves the new one, read past a warm cache); devices (MR007-SCOPE: a new `binding_id` per register; unregister with an old one 409, the row stays); `account-card.test.ts` (MR007-CARD: the vector card verifies, every `refused_cards` entry is refused, a stored version 3 card is not served); support worker (its calls and card name its relay's PeerId).

**pp-browser:** `relay_sign_vectors_test.cpp` (MR007-VEC; writes the file); the builders' own tests (`registration_sign_payload_test`, `relay_message_sign_payload_test`, `relay_blob_sign_payload_test`, `relay_file_sign_payload_test`, `http_locker_client_test`) moved to the new versions (MR007-AUD); a pin store test (MR007-PIN: first fetch pins, an invalid answer pins nothing, another PeerId → changed, a PeerId held by another origin → refused, a card writes nothing, our own card on a linked device does, the 32-pin bound keeps ours and the org's, a newer `schema_version` is refused); HTTP clients put the pinned audience in body and bytes, and `wrong_audience` triggers one identity fetch (MR007-PIN); `account_card_test` (MR007-CARD: sign, verify, bounds, version 3 refused, the golden vector) and the `profile.db` 7 → 8 step; `RelaySendCoordinator` (MR007-SEND: a card entry conflicting with a pin is skipped, `wrong_audience` tries the next relay, counts toward the stale-list fetch and leaves `RelayHealth` alone); revoke and push (MR007-SCOPE: the id sent, `registration_mismatch` / `binding_mismatch` → renew or register, then once more); `RelayRegistrationsWorkflow` and the identity's registration (MR007-HEAL: due now, backoff, not while leaving or changed); My relays (Add's identity errors, *identity changed* left off the card, Accept re-pins and re-issues). **pp-node:** heartbeat and registration carry the audience; a config pin wins; a changed identity stops relay calls and logs once (MR007-PIN).

**App lab** (on the local pp-relay-apis stacks A and B, each its own identity key, behind the lab's TLS fronts; purpose **B-RELAY-AUDIENCE**), scenario `replayed_calls_refused_at_other_relay`:

1. Alice is on A and B (A preferred, both pinned); Bob, on A, sends Alice three messages while her app is not polling (her probe pauses sync), so they wait in her inbox on A.
2. B's TLS front records the request bodies Alice's app sends to B, then Alice's app, through the probe: polls B; clears B's inbox with a `before` of now (a probe-only argument; the product clears only mail at least a day old, which would prove nothing); removes B from My relays (its revoke goes to B). The harness checks each recorded body's `audience` is B's PeerId (from B's `GET /v1/relay-identity`) and not A's.
3. Within the 5-minute window the harness posts each recorded body verbatim to the same path on A: every one answers `401 wrong_audience`.
4. The same poll body with `audience` rewritten to A's PeerId answers `401` (signature) at A: the audience is signed, not only checked.
5. Control: a poll body recorded on the way to A, replayed at A, is accepted (the harness's replay works, so the refusals in 3 are the audience's).
6. Alice is still registered on A (`GET /v1/users/by-account` at A finds her), her app resumes sync and receives Bob's three messages from A, and she and Bob still exchange a message.

The lab never calls these replays from product code; the TLS front's recording is a lab-only switch.

## Privacy

- **A relay list tells whoever holds the card which relays the user uses.** Cards go to contacts, to group members and to the relays the user registers on. That is the same audience that already learns the user's devices. The list carries no IP address.
- **A relay learns the Account IDs of the senders that write to its users.** That is already true for its own users; a foreign sender is now visible too.

## Not in scope

- Relay-to-relay forwarding or a registry of relays.
- Choosing relays automatically. The user lists them; the app may suggest the org's relays.
- Search across relays by name. A name search asks the relays the user has listed.
