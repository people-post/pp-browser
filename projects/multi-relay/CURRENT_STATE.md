# Multi-relay — current state

**As of:** 2026-11-13 (R2 step 3 built)

Design written ([DESIGN.md](DESIGN.md)); MR001–MR005 accepted.

**R0 built** (app only, no wire change): the contact page's *Refresh account info* is on every contact's page
(phone row and desktop button; no longer behind *advanced* or a `relay:` id). It runs
`ContactRefreshWorkflow` (`capabilities/messaging/src/feature/conversations/`): the contact's devices are asked
for the account card even when one is kept (`ContactCardExchange::AskDevices`, at most once per account per
15 s), then our directory by Account ID (since R2 step 1 the only id it knows; a contact with none asks no one) on a worker; the
merge and the write land on the UI thread (`AppRuntime::PostUI`), the button reads "Refreshing…" meanwhile, and
a second press does nothing. Keys are kept only through `AccountKeyResolver` (`VerifyKeyCard` /
`VerifyKemBinding`: the signing key hashes to the Account ID, the KEM key carries its binding); a directory
answer whose keys do not verify is used for name and photo only, one for another account not at all, and the
user is told. The toast says what changed (name, photo, keys, devices), "Already up to date", or which source
could not be reached. Contract text: [ACCOUNT_CARD § Card exchange](../../docs/contracts/ACCOUNT_CARD.md#card-exchange-over-amp).

Not yet in R0's scope (later phases): the contact's own relays (R2's relay list) and our other relays are not
asked, and the page does not show the preferred relay (R3). DESIGN's "Where it stands" still describes the old
button.

**R1 (relays) merged, not deployed:** nfsc-brief-reader/app-static-api#252 and nfsc-brief-web3/app-messenger#28 (to
`develop`).

**R2 step 1 (app) merged, not deployed** (#609; on main, pp-node 8.0.0): one relay as before, addressed by Account ID
everywhere (no `relay:` ids, relay API sign version 2); grants (MR005) issued, delivered over the control lane,
renewed and attached; the relay's new errors handled; calls grant their people. Contracts: [SERVICE_ENDPOINTS](../../docs/contracts/SERVICE_ENDPOINTS.md),
[ACCOUNT_CARD § Relay grants](../../docs/contracts/ACCOUNT_CARD.md#relay-grants), [WIRE_SCHEMAS](../../docs/contracts/WIRE_SCHEMAS.md),
[DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md), [COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md#clean-cutovers-pre-release).
Needs pp-cpp-mesh v9.0.0 (directory rows by Account ID).

**R2 step 2 (#612, relay nfsc-brief-reader/app-static-api#254), merged, not deployed:** account card version 3
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

**R2 step 3 (branch `feat/relay-clients`), built, not merged; app only, no wire or relay change:** our other relays
come from config (`relay.extra_base_urls`, at most 4 relays in all; no UI until R3) and go on our card after the
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

Not yet:
- On the preferred relay only: profile photos and blobs, chat attachment files, account lockers, stream history
  (`FetchChatHistory`), the directory (search and lookups; a send's stale-card fetch also asks our other relays'
  directories), client compatibility, the Brief LLM key and the backend switch's push unbinding.
- What another relay answered at registration is kept in memory only (each start registers there again); the Me page
  shows the preferred relay's registration only (R3).
- A card with no relay of ours that takes https (an `http://` dev relay) says the mesh only, so senders fall back to
  their own preferred relay, as before.
- The pricing UI still takes a peer's floor from the directory hit and `charge_required`, not from its card; the
  contact page does not show the relays (R3).

Deploy (clean cutover): the pp-node fleet runs 8.0.0 (pp-relay-infra-ansible#20); the relays (R1 and step 2) and the
TestFlight build with step 2 switch together. Step 3 needs no relay change.

Next: R3 (Me → Network → My relays; the contact page's relays).
