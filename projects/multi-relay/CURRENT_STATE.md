# Multi-relay — current state

**As of:** 2026-11-15 (R3, R4, R5 and R6 built and run in the app lab on two local pp-relay-apis stacks; not merged or deployed)

Design written ([DESIGN.md](DESIGN.md)); MR001–MR008 accepted.

**R0 built** (app only, no wire change): the contact page's *Refresh account info* is on every contact's page
(phone row and desktop button; no longer behind *advanced* or a `relay:` id). It runs
`ContactRefreshWorkflow` (`capabilities/messaging/src/feature/conversations/`): the contact's devices are asked
for the account card even when one is kept (`ContactCardExchange::AskDevices`, at most once per account per
15 s), then our directory by Account ID (since R2 step 1 the only id it knows; a contact with none asks no one) on a worker; the
merge and the write land on the UI thread (`AppRuntime::PostUI`), the button reads "Refreshing…" meanwhile, and
a second press does nothing. Keys are kept only through `AccountKeyResolver` (`VerifyKeyCard` /
`VerifyKemBinding`: the signing key hashes to the Account ID, the KEM key carries its binding); a relay answer
that does not verify (its keys or card, or it is for another account) contributes nothing and gives way to the next
relay, and the user is told. The toast says what changed (name, photo, keys, devices), that their card is already up to date,
or that nothing could be reached or found. Contract text: [ACCOUNT_CARD § Card exchange](../../docs/contracts/ACCOUNT_CARD.md#card-exchange-over-amp).

R3 extends the Refresh with the contact's relays and ours (below).

**R1 (relays) merged and deployed:** nfsc-brief-reader/app-static-api#252 and nfsc-brief-web3/app-messenger#28 (to
`develop`).

**R2 step 1 (app) merged** (#609; on main, pp-node 8.0.0): one relay as before, addressed by Account ID
everywhere (no `relay:` ids, relay API sign version 2); grants (MR005) issued, delivered over the control lane,
renewed and attached; the relay's new errors handled; calls grant their people. Contracts: [SERVICE_ENDPOINTS](../../docs/contracts/SERVICE_ENDPOINTS.md),
[ACCOUNT_CARD § Relay grants](../../docs/contracts/ACCOUNT_CARD.md#relay-grants), [WIRE_SCHEMAS](../../docs/contracts/WIRE_SCHEMAS.md),
[DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md), [COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md#clean-cutovers-pre-release).
Needs pp-cpp-mesh v9.0.0 (directory rows by Account ID).

**R2 step 2 (#612, relay nfsc-brief-reader/app-static-api#254), merged, the relay deployed:** account card version 3
signs `relays` (preferred first) and `initiation_floor`, with MR004's bounds refused whole on both sides; one golden
vector for the sign bytes in both repos; version 2 refused everywhere. Our card lists the relay we register on (the
registration base URL; an `http://` lab relay is left off, so the lab's cards say the mesh only) and our floor; the
relay list is the main device's, the floor the account's (a sibling's newer card sets it, as the nickname). The card
goes at every `register/finish`, and again when it is newer than the one the relay last answered for; the
registration's floor field is gone. The relay stores the card, serves it from `GET /users/by-account` and reads the
stranger floor from it (402 with the card's floor). A contact keeps the newest list and floor with its card
(`KeptRelays` / `KeptInitiationFloor`, read only). `profile.db` 7 steps kept version 2 cards: ours keeps its device
list and is re-issued on first use, others' are asked for again. Contracts: [ACCOUNT_CARD](../../docs/contracts/ACCOUNT_CARD.md#account-card),
[SERVICE_ENDPOINTS](../../docs/contracts/SERVICE_ENDPOINTS.md), [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md#schema-versions),
[COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md#clean-cutovers-pre-release).

**R2 step 3 (#614), merged; app only, no wire or relay change:** our other relays
came from config (`relay.extra_base_urls`, at most 4 relays in all; since R3 the profile's list, below) and go on our card after the
preferred one (the https ones). Each gets its own message, push and registration clients (`BackendClientSet`); once
registered on the preferred relay the account registers on the others with its card, again for a newer card, near the
expiry and after a backoff, and revokes there too (`RelayRegistrationsWorkflow`). The preferred inbox is polled at the
app's pace, the others at every 4th sync and on every wake, each from its own cursor (`relay_inbox_cursor.json`
`relays`, additive) and acked where the batch came from (`RelayInboxCoordinator`); a message that arrives twice is
dropped by its id. Push tokens go to every relay holding our registration. Sends go into the recipient's relays in its
card's order, a relay that just failed tried last for 5 min, no card → our preferred relay; `recipient_not_registered`
everywhere fetches the newest card (devices, then our relays' and directories' stored cards) and tries its list once
more (`RelaySendCoordinator`, `RecipientRelays`). The lab runs a second relay stack and TLS in front of both (a lab CA,
`PP_BROWSER_CA_BUNDLE` on desktop builds): `users_on_two_relays_meet`, `preferred_relay_down_mail_still_arrives`.
Contracts: [SERVICE_ENDPOINTS § Several relays](../../docs/contracts/SERVICE_ENDPOINTS.md#several-relays),
[ACCOUNT_CARD](../../docs/contracts/ACCOUNT_CARD.md#account-card), [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md),
[CONFIGURATION](../../docs/ops/CONFIGURATION.md).

**R3 and R4 (branch `feat/my-relays`), built, not merged; app only, no wire or relay change** (the locator's
`relay` is an optional field old apps ignore):
- MR006: the user's relay services follow the preferred relay (inbox, push, the identity's registration, the
  directory, profile photos, blobs and chat attachment files); org services stay on the org's origin (the Brief LLM
  gateway and its key, Support, diagnostics, crash reports, client compatibility). `BackendClientSet::Apply` is the
  one place that splits them; a user who never edits the list sees no change.
- My relays are the profile's (`our_relays.json`, seeded once from the deployment's relay and
  `relay.extra_base_urls`), each with its last registration, read back at start (a relay holding our registration
  for the current card is not asked again). Inbox cursors are kept by relay (`relay_inbox_cursor.json`
  `cursor_relay`).
- Me → My relays (a page of its own on Home, after My devices; phone and desktop): the list, preferred first, each
  with its registration and last poll; Add relay… (https; the relay must answer `client-compat`), Make preferred
  (the identity registers there, #602's path; push binds there), Remove… (never the preferred or the last; push and
  registration revoked there first). Every change re-issues the card and sends it to contacts
  (`MyRelaysWorkflow`).
- Search and pricing: a name search asks every relay of ours and merges the answers, a lookup asks each in turn
  (`OurRelaysDirectory`); a stranger's price is its card's `initiation_floor`, the relay's 402 over a card it came
  after (`InitiationBillingStore`).
- The contact page shows the contact's preferred relay by host with its reachability at the last Refresh; the
  Refresh asks its devices, then its relays in list order, then ours (`ContactRefreshWorkflow`).
- R4: the QR locator carries our preferred relay (`relay`, MR004's bounds); a contact added from one starts with it
  as its relay list (`contacts.json` `remote.locator_relay`), and its card comes from that relay when no device
  answers.
- Lab: `preferred_relay_switched_mail_still_arrives`, `invite_names_their_relay`, with step 3's two.

Contracts: [SERVICE_ENDPOINTS § Relay services and org services](../../docs/contracts/SERVICE_ENDPOINTS.md),
[ACCOUNT_CARD](../../docs/contracts/ACCOUNT_CARD.md), [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md),
[CONFIGURATION](../../docs/ops/CONFIGURATION.md), [SCANNING](../../docs/ui/SCANNING.md),
[UI_DESIGN_SYSTEM § My relays](../../docs/ui/UI_DESIGN_SYSTEM.md).

**R5 (MR007, branch `feat/relay-audience`; the relay on pp-relay-apis `feat/relay-identity-audience`), built, not
merged; a clean cutover with the relay:** every signed relay command names its relay's PeerId (the audience) at a new
sign version; the app pins each origin to the PeerId its own `GET /v1/relay-identity` answered (`relay_pins.json`) before
it signs for it, treats another PeerId after `wrong_audience` as *identity changed* (nothing signed until the person
accepts it in My relays; R6 replaced both: the PeerId comes from the relay's address, with nothing to accept), and signs a send into a contact's relay for the PeerId its card (version 4, `profile.db` 8)
names; revoke and unregister name the registration and binding they remove; `not_registered` from a relay of ours
registers there again. pp-node pins its relay the same way. The golden vectors (`relay_sign_vectors.json`, written by
`relay_sign_vectors_test.cpp`) pass pp-relay-apis' own vector and card tests. Contracts:
[SERVICE_ENDPOINTS § Relay identity and the audience](../../docs/contracts/SERVICE_ENDPOINTS.md#relay-identity-and-the-audience-multi-relay-mr007),
[ACCOUNT_CARD](../../docs/contracts/ACCOUNT_CARD.md#account-card), [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md#relay_pinsjson),
[COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md#clean-cutovers-pre-release),
[UI_DESIGN_SYSTEM § My relays](../../docs/ui/UI_DESIGN_SYSTEM.md#my-relays). The lab scenario
`replayed_calls_refused_at_other_relay` runs green on the lab's pp-relay-apis local stacks with MR007 (pp-relay-apis
`feat/relay-identity-audience`, each stack its own identity), as do the other multi-relay scenarios.

**R6 (MR008, on `feat/relay-audience` with R5; pp-relay-apis `feat/relay-identity-audience` prints and names the
relay link), built, not merged; ships in R5's cutover, no relay change:** a relay address is its origin and its
PeerId. The app never learns a PeerId from a relay's answer: the org's ships with the build (`PP_RELAY_PEER_ID_PROD` /
`_QA`) or the custom config (`relay.peer_id`); a relay the person adds comes as a relay link (`https://host#<PeerId>`,
pasted or scanned; a bare origin in developer builds only, pinned on first use); `our_relays.json` 2 keeps each relay's
PeerId; a linked device takes our card's, a restore the locker's; the QR locator carries `{origin, peer_id}`. The pins
are expected identities (`relay_pins.json` 2): `GET /v1/relay-identity` is only checked (once per run, after a
`wrong_audience`, at Add), and a relay whose answer names another PeerId is *not used* (no Accept). A link naming a
PeerId held at another origin is refused, the org's written first. First use only in developer builds (and for the
compiled backend of a dogfood build shipping no PeerId): a release build's custom backend or `PP_BROWSER_ORIGIN`
without its relay's PeerId does not use that relay (Me → Network says so). pp-node expects `registration.peer_id`
(`PP_NODE_REGISTRATION_PEER_ID`; the compiled prod PeerId with the compiled URL, however spelled); a release build
without one refuses to register. Release workflows fail without
`PP_RELAY_PEER_ID_PROD` (`scripts/check/check_relay_peer_ids.sh`). My relays shows **Copy link**. Lab:
`relay_not_matching_its_link_unused` (B-RELAY-LINK) and the multi-relay scenarios on relay links. Contracts:
[SERVICE_ENDPOINTS § Pins](../../docs/contracts/SERVICE_ENDPOINTS.md#pins),
[ACCOUNT_CARD § Locator](../../docs/contracts/ACCOUNT_CARD.md#locator-qr-code-invite-link),
[DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md#relay_pinsjson),
[COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md#clean-cutovers-pre-release),
[BUILD § Backend origin](../../docs/ops/BUILD.md#backend-origin), [CONFIGURATION](../../docs/ops/CONFIGURATION.md).

Not yet:
- A relay link shown as a QR code (My relays copies the link; Add relay… scans one).
- A signed rotation statement for the org relay's key (a rotation is an app release meanwhile).
- Account lockers stay at the org's registration URL and are still written there; with the org's relay removed from
  the list (our registration there revoked) those writes fail (logged), the restore's read still works. Stream history
  is read from the org's relay while it is one of ours, else from the preferred one (the org relay's is then not
  asked).
- A custom `registration.base_url` that differs from `relay.base_url` now only sets where lockers live.
- The list can never be empty (no mesh-only choice).
- Only the main device edits the list. A linked device's page is read-only; it follows a newer card of ours, but
  does not catch up at start when the two already differ.
- A relay's reachability is its last poll, in memory only, shown without a time.
- A card with no https relay of ours (an `http://` dev relay) still says the mesh only, so senders fall back to
  their own preferred relay.

Deploy: the relays (R1 and step 2) and the pp-node fleet on 8.0.0 (pp-relay-infra-ansible#20) are deployed. Steps 3,
R3 and R4 need no relay change; they ship with the app. R5/R6 before the prod window: the repository variables
`PP_RELAY_PEER_ID_PROD` and `PP_RELAY_PEER_ID_QA` (each environment's relay PeerId, from infra's record of its key) for
`release.yml`, `ios-testflight.yml` and `release-pp-node.yml`; QA pp-nodes need `PP_NODE_REGISTRATION_PEER_ID` set to
the QA relay's PeerId (`pp-relay-infra-ansible`, Leo); prod nodes take the compiled one. A user-run node (the g-ipfs
Pi) built without `PP_API_URL_PROD` / `PP_RELAY_PEER_ID_PROD` needs `PP_NODE_REGISTRATION_PEER_ID` in its env before it
runs an R6 build, or it stops registering.

Next: the items above, as they matter; the multi-relay phases are otherwise done.
