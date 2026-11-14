# Multi-relay: phases

Order of work. Status is in [CURRENT_STATE.md](CURRENT_STATE.md).

## R0 — The refresh button, app only (no wire change)

- [x] Off the UI thread: the button posts to a worker, and the result comes back through `AppRuntime::PostUI`.
- [x] Ask the contact's devices first (the card exchange), then the directory by Account ID; accept keys only when they verify against the Account ID.
- [x] Show the button for every contact (not only those with a `relay:` id); say what changed and what could not be reached.
- [x] Tests: the order of sources, key verification, the worker path.

## R1 — Relays: inboxes by Account ID, account-signed sends (relay first: `www`, `messenger`)

- [x] Registration keeps one row per Account ID, without a `relay:` id, and stores the verified account card.
- [x] `GET /users/by-account/{account_id}` returns the row and its card; `GET /users/{relay_user_id}` is removed.
- [x] `POST /messages` by Account IDs; the recipient must be registered (`recipient_not_registered`); a foreign sender carries its key, checked against its Account ID; a per-IP rate for foreign senders.
- [x] Polling, ack and clear by Account ID; push registration unchanged per relay.
- [x] MR005 at the relay: a grant on a send checked with the recipient's key; a stranger's send refused with `payment_required` when the recipient's card has a floor above 0, else within a first-contact quota per recipient and without a message alert; the per-address limit configurable.
- [x] `messenger` records keyed by Account ID.
- [x] Deploy with the app build from R2 (clean cutover): merged (#252, #28) and deployed.

## R2 — App: Account ID everywhere, the relay list, several relays

- [x] `ContactIdKind::RelayUser` and `relay:` ids removed (routes, envelopes, contacts, directory hits, registration, history and stream ids, devices, blobs, files, lockers, profile, revoke, the node heartbeat, the contact refresh); stored ones mapped or dropped (`profile.db` 6, `thread.db` 4, `contacts.json` 3, `identity.enc` 4, `initiation_billing.json` 2).
- [x] The account card's `relays` field (sign, verify, bounds; card version 3, app and relay); a contact keeps the newest list with its card (`KeptRelays`), the kept version 2 cards stepped (`profile.db` 7); ours lists the relay we register on and goes at `register/finish`.
- [x] Clients per relay: register, poll (preferred at pace, others every 4th sync and on every wake), push tokens on each; our other relays from `relay.extra_base_urls` until R3 (step 3).
- [x] MR005 grants in the app: issued to contacts and group members (and, unrenewed, to the people of a call we answer or are in) over the control lane, acked, renewed, attached to sends, not renewed once blocked or removed; `grant_expired` / `invalid_grant` ask again and send once as a stranger; `payment_required` shows the receiver's price; `stranger_quota_exceeded` retried with backoff, and what it held back goes once the grant arrives.
- [x] MR005: `initiation_floor` in the account card (`KeptInitiationFloor`); the relay reads a stranger's price from the card it registered with; no floor field at registration.
- [x] A send the relay answers `sender_key_required` goes again with our key; `recipient_not_registered` is a plain failure (then Amp, when the peer has a link).
- [x] Sending in list order with a per-relay health memory; `recipient_not_registered` fetches the card and retries once (step 3).
- [x] Contracts promoted: ACCOUNT_CARD, SERVICE_ENDPOINTS, WIRE_SCHEMAS, COMPATIBILITY rows (steps 1 and 2: Account IDs, grants, the errors, card version 3; step 3: SERVICE_ENDPOINTS § Several relays and § Sending by the list, no wire change).
- [x] Lab: two relays behind TLS; users registered on different ones message and call each other; the preferred relay stopped mid-run (`users_on_two_relays_meet`, `preferred_relay_down_mail_still_arrives`).

## R3 — UI

- [x] My relays: list, add, remove, mark preferred, each with its state (Me → My relays, a page of its own; MR006 splits relay services from org services).
- [x] Contact page: the preferred relay and its reachability; the refresh button's final form (devices, their relays, ours).
- [x] Search asks every relay of ours; a stranger's price comes from its card.

## R4 — QR locator

- [x] The locator may carry the preferred relay's origin (MR004); a contact added from it starts with that relay.

## R5 — Relay identity and audience

[MR007](DECISIONS.md#mr007--relays-have-a-node-style-identity-every-signed-relay-command-names-it-as-its-audience); spec [DESIGN § Relay identity and audience](DESIGN.md#relay-identity-and-audience-mr007). Clean cutover: relay on qa first, then the app, pp-node and prod together.

**Relay (pp-relay-apis; Leo reviews)**
- [ ] Identity: `RELAY_IDENTITY_PUBLIC_KEY` on every signing / verifying function, `RELAY_IDENTITY_KEY_SECRET_ID` on the registry (the seed checked against the public key); `GET /v1/relay-identity`; `503 relay_identity_unavailable` on everything signed when misconfigured; `scripts/relay-identity-keygen.mjs`.
- [ ] Audience in `versioned()`, `relayApiHeader()` and the registration builder (the version table); the body's `audience` checked first in the verifier and in `registerFinish` (before the challenge is consumed): `401 wrong_audience`.
- [ ] `registration_id` answered by `register/finish`; revoke v2 names it, removes conditionally on a consistent read, `409 registration_mismatch` otherwise.
- [ ] `binding_id` answered by `devices/register`; unregister (op 4) names it, removes conditionally, `409 binding_mismatch` otherwise.
- [ ] Account card version 4 (verify, bounds, `invalid_card_relays`); version 3 refused at registration and a stored one not served.
- [ ] Support worker: its relay's PeerId from `RELAY_IDENTITY_PUBLIC_KEY` as audience; its card v4.
- [ ] `relay-sign-vectors.json` copied from pp-browser; the tests of DESIGN § Tests; `openapi/relay.yaml`, `SPECS.md`, `docs/architecture.md` updated.

**Infra (Leo, pp-relay-infra-terraform)**
- [ ] A key per environment (`qa`, `prod`) from the keygen script: the seed in Secrets Manager, the public key and PeerId recorded in the repo; the variables on the functions and `GetSecretValue` for the registry. In place before the relay deploys.

**App (pp-browser)**
- [ ] Builders for every domain at its new version with the audience (registration, revoke, profile, icon, heartbeat, LLM token, locker put, blobs, files incl. status, relay API v3 incl. unregister's `binding_id`); `relay_sign_vectors_test.cpp` writes and checks `relay_sign_vectors.json`.
- [ ] `relay_pins.json` and the pin rules (fetch, one PeerId per origin and one origin per PeerId, never from others' cards, our own card on a linked device, 32 at most); pinned before the first signed call on each relay of ours and on the org's origin; first start after the upgrade pins them all.
- [ ] Every HTTP client sends its origin's pinned audience; `wrong_audience` fetches the identity once and sets *identity changed* when it differs.
- [ ] Card version 4: `relays: [{origin, peer_id}]` signed, verified, bounded; version 3 refused everywhere; `KeptRelays` with PeerIds; `profile.db` 7 → 8; ours lists only pinned relays and is re-issued and announced after the upgrade.
- [ ] Sending: a contact's card entry names the audience; a pin conflict or `wrong_audience` skips the relay (stale-list fetch, no health mark); a locator-only contact pins by fetch.
- [ ] Revoke with `registration_id` (kept per registration; `registration_mismatch` → renew, revoke once); unregister with `binding_id` (in memory; `binding_mismatch` → register, unregister once).
- [ ] Self-heal: `not_registered` / `registration_expired` from a relay of ours where we believe we are registered re-registers (the preferred one whatever auto-renew), with backoff.
- [ ] My relays: Add's identity errors and the re-pin confirm; *Identity changed — not used* with **Accept new identity…**; a changed relay left off the card; the org origin's line; the contact page's *identity doesn't match*.
- [ ] Contracts promoted in the same PR: SERVICE_ENDPOINTS (identity endpoint, the audience rule and table, `wrong_audience`, the scoped commands, self-heal), ACCOUNT_CARD (version 4), DATA_LAYOUT (`relay_pins.json`, `our_relays.json` `registration_id`, `profile.db` 8), COMPATIBILITY (the cutover rows), UI_DESIGN_SYSTEM (My relays states); MR007's Status updated.

**pp-node**
- [ ] Registration, heartbeat and revoke carry the audience: the relay's PeerId fetched and pinned in the data directory (no config key); a changed identity logs and stops relay calls.
- [ ] Released with the app (the fleet on it in the prod window).

**Lab**
- [ ] The local pp-relay-apis stacks A and B with their own identity keys; the TLS front's request recording (lab-only).
- [ ] Probe: pause and resume sync, clear with an explicit `before`, remove a relay (exists), read the relay identity.
- [ ] `replayed_calls_refused_at_other_relay` (B-RELAY-AUDIENCE) green; the multi-relay scenarios (`users_on_two_relays_meet`, `preferred_relay_down_mail_still_arrives`, `preferred_relay_switched_mail_still_arrives`, `invite_names_their_relay`) green on the new stacks.

**Deploy**
- [ ] Relay to `qa` (after infra); app and pp-node branch builds tested against it.
- [ ] Prod window: app build available, relay release to `main` with `client-compat` `min_client_version` raised to the R5 build, pp-node fleet (`pp-relay-infra-ansible`, Leo).
