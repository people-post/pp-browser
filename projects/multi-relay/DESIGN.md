# Multi-relay: design

**Status:** Proposed (2026-11-12), decided in outline ([MR001](DECISIONS.md#mr001--inboxes-are-keyed-by-account-id)–[MR004](DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)); relay identity and audience built ([MR007](DECISIONS.md#mr007--relays-have-a-node-style-identity-every-signed-relay-command-names-it-as-its-audience), R5; the contracts hold it). Pre-release: every change below is a clean cutover ([COMPATIBILITY § No bridges](../../docs/contracts/COMPATIBILITY.md#no-bridges-pre-release)).

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
  { "origin": "https://relay.example.org/api/relay", "peer_id": "Qm…" },
  { "origin": "https://www.brief.global/api/relay", "peer_id": "Qm…" }
]
```

- Ordered: the first entry is the **preferred** relay. An empty list means the account receives over the mesh only.
- Card version 4 (MR007): each entry also names the relay's PeerId — [ACCOUNT_CARD § Account card](../../docs/contracts/ACCOUNT_CARD.md#account-card).
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

[MR007](DECISIONS.md#mr007--relays-have-a-node-style-identity-every-signed-relay-command-names-it-as-its-audience).
**Built** (R5; app and pp-node on `feat/relay-audience`, the relay on pp-relay-apis `feat/relay-identity-audience`), not
deployed. What holds is in the contracts, the one editable copy:
[SERVICE_ENDPOINTS § Relay identity and the audience](../../docs/contracts/SERVICE_ENDPOINTS.md#relay-identity-and-the-audience-multi-relay-mr007)
(the identity endpoint, pins, the audience rule and its version table, `wrong_audience`, the scoped revoke and
unregister, self-heal, the golden vectors), [ACCOUNT_CARD § Account card](../../docs/contracts/ACCOUNT_CARD.md#account-card)
(version 4), [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md#relay_pinsjson) (`relay_pins.json`, `profile.db` 8),
[COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md#clean-cutovers-pre-release) (the cutover rows) and
[UI_DESIGN_SYSTEM § My relays](../../docs/ui/UI_DESIGN_SYSTEM.md#my-relays). What stays here is the relay's side of the
work, the rollout and the tests, as planned.

**Threat:** a relay on a user's list (honest-but-curious or hostile) replaying the user's signed calls at the user's other relays, or at any relay the user's contacts use. Not a network attacker: TLS covers that. Not the relay misusing its own copy: a relay can already do anything to its own data.

### The relay's side (pp-relay-apis)

**Key.** Each relay deployment (`qa`, `prod`, each lab stack) has its own ML-DSA-65 key pair. The secret is the 32-byte FIPS 204 key-generation seed (ξ), stored as plain text base64 in the relay's Secrets Manager secret. Its PeerId is `pp::PeerIdFromMlDsaPublicKey(public key)` ([MESH_IDENTITY](../../docs/architecture/MESH_IDENTITY.md#derivation-pipeline-frozen): `08 04 12 A0 0F ‖ key` → `12 20 ‖ SHA-256` → base58btc, `Qm…`); pp-relay-apis' `peerIdFromMlDsaPublicKey` already matches it.

| Variable | Functions | Value |
|---|---|---|
| `RELAY_IDENTITY_PUBLIC_KEY` | registry, messenger, storage, push, support-worker (every function that verifies or makes a signed relay call) | standard base64 of the raw 1952-byte public key (public, a plain variable); each derives the PeerId once per instance |
| `RELAY_IDENTITY_KEY_SECRET_ID` | registry only | Secrets Manager id of the seed (`secretsmanager:GetSecretValue` on it, registry only; cached 15 min per instance like the LLM token key) |

- The registry checks once per instance that the secret's seed generates `RELAY_IDENTITY_PUBLIC_KEY`. Nothing else uses the private key in this step; it exists now so that the identity is real when sealing (MR007 item 6) or Amp needs it, and so no function but the registry can read it.
- **Missing or inconsistent configuration fails closed:** a function without a valid `RELAY_IDENTITY_PUBLIC_KEY` answers every signed call, and the registry `GET /v1/relay-identity` too, with `503 relay_identity_unavailable` (and the config check reports the missing variable as the others do, `ConfigError`). A relay never accepts a call without checking its audience.
- **Key generation:** a script in pp-relay-apis (`scripts/relay-identity-keygen.mjs`, `@noble/post-quantum`'s `ml_dsa65.keygen(seed)` from 32 random bytes) prints the seed (for the secret) and the public key and PeerId (for the variable and the infra repo's record). Local and lab stacks run the same script at setup and give each stack its own key through the same secret mechanism the local stack uses for the LLM token signing key; two lab stacks must never share a key.
- **Rotation is a new relay.** A new key is a new PeerId: every app shows that relay as *identity changed* and stops using it until its user accepts the new one; every contact's card that names the old PeerId is skipped by senders until its owner accepts and re-issues; every pp-node stops its relay calls until its pin is cleared. So a relay's key is rotated only after a compromise, and before sealing ships nothing checks a signature by it, so a leaked seed alone harms no one yet.

pp-relay-apis' `createSignedRequestVerifier` (every registered-key call) and `registerFinish`:

1. Parse the JSON body.
2. **`audience`** — absent, not a string, or not byte-equal to this relay's PeerId → `401 { "error": "Signed for another relay", "code": "wrong_audience" }`. Checked first: before the timestamp window, the account lookup, the signature, and (for `register/finish`) before the challenge is consumed, so a misdirected finish does not spend the challenge.
3. Everything else as today (fields, window, lookup, expiry, signature), the sign bytes built with this relay's PeerId.

An old client (no `audience` in the body) gets `wrong_audience`, never a confusing signature failure. A body whose `audience` was rewritten to this relay's PeerId fails the signature (`401 Signature verification failed`).

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

**pp-browser:** `relay_sign_vectors_test.cpp` (MR007-VEC; writes the file); the builders' own tests (`registration_sign_payload_test`, `relay_message_sign_payload_test`, `relay_blob_sign_payload_test`, `relay_file_sign_payload_test`, `http_locker_client_test`) moved to the new versions (MR007-AUD); a pin store test (MR007-PIN: first fetch pins, an invalid answer pins nothing, another PeerId → changed, a PeerId held by another origin → refused, a card writes nothing, our own card on a linked device does, the 32-pin bound keeps ours and the org's, a newer `schema_version` is refused); HTTP clients put the pinned audience in body and bytes, and `wrong_audience` triggers one identity fetch (MR007-PIN); `account_card_test` (MR007-CARD: sign, verify, bounds, version 3 refused, the golden vector) and the `profile.db` 7 → 8 step; `RelaySendCoordinator` (MR007-SEND: a card entry conflicting with a pin is skipped, `wrong_audience` tries the next relay, counts toward the stale-list fetch and leaves `RelayHealth` alone); revoke and push (MR007-SCOPE: the id sent, `registration_mismatch` / `binding_mismatch` → renew or register, then once more); `RelayRegistrationsWorkflow` and the identity's registration (MR007-HEAL: due now, backoff, not while leaving or changed); My relays (Add's identity errors, *identity changed* left off the card, Accept re-pins and re-issues). **pp-node:** heartbeat and registration carry the audience; a changed identity stops relay calls and logs once (MR007-PIN).

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
