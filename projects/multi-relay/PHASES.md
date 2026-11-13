# Multi-relay: phases

Order of work. Status is in [CURRENT_STATE.md](CURRENT_STATE.md).

## R0 — The refresh button, app only (no wire change)

- [x] Off the UI thread: the button posts to a worker, and the result comes back through `AppRuntime::PostUI`.
- [x] Ask the contact's devices first (the card exchange), then the directory by Account ID; accept keys only when they verify against the Account ID.
- [x] Show the button for every contact (not only those with a `relay:` id); say what changed and what could not be reached.
- [x] Tests: the order of sources, key verification, the worker path.

## R1 — Relays: inboxes by Account ID, account-signed sends (relay first: `www`, `messenger`)

- [ ] Registration keeps one row per Account ID, without a `relay:` id, and stores the verified account card.
- [ ] `GET /users/by-account/{account_id}` returns the row and its card; `GET /users/{relay_user_id}` is removed.
- [ ] `POST /messages` by Account IDs; the recipient must be registered (`recipient_not_registered`); a foreign sender carries its key, checked against its Account ID; a per-IP rate for foreign senders.
- [ ] Polling, ack and clear by Account ID; push registration unchanged per relay.
- [ ] MR005 at the relay: a grant on a send checked with the recipient's key; a stranger's send refused with `payment_required` when the recipient's card has a floor above 0, else within a first-contact quota per recipient and without a message alert; the per-address limit configurable.
- [ ] `messenger` records keyed by Account ID.
- [ ] Deploy with the app build from R2 (clean cutover).

## R2 — App: Account ID everywhere, the relay list, several relays

- [x] `ContactIdKind::RelayUser` and `relay:` ids removed (routes, envelopes, contacts, directory hits, registration, history and stream ids, devices, blobs, files, lockers, profile, revoke, the node heartbeat, the contact refresh); stored ones mapped or dropped (`profile.db` 6, `thread.db` 4, `contacts.json` 3, `identity.enc` 4, `initiation_billing.json` 2).
- [x] The account card's `relays` field (sign, verify, bounds; card version 3, app and relay); a contact keeps the newest list with its card (`KeptRelays`), the kept version 2 cards stepped (`profile.db` 7); ours lists the relay we register on and goes at `register/finish`.
- [ ] Clients per relay: register, poll (preferred at pace, others slower), push tokens on each.
- [x] MR005 grants in the app: issued to contacts and group members (and, unrenewed, to the people of a call we answer or are in) over the control lane, acked, renewed, attached to sends, not renewed once blocked or removed; `grant_expired` / `invalid_grant` ask again and send once as a stranger; `payment_required` shows the receiver's price; `stranger_quota_exceeded` retried with backoff, and what it held back goes once the grant arrives.
- [x] MR005: `initiation_floor` in the account card (`KeptInitiationFloor`); the relay reads a stranger's price from the card it registered with; no floor field at registration.
- [x] A send the relay answers `sender_key_required` goes again with our key; `recipient_not_registered` is a plain failure (then Amp, when the peer has a link).
- [ ] Sending in list order with a per-relay health memory; `recipient_not_registered` fetches the card and retries once.
- [ ] Contracts promoted: ACCOUNT_CARD, SERVICE_ENDPOINTS, WIRE_SCHEMAS, COMPATIBILITY rows (steps 1 and 2 promoted: Account IDs, grants, the errors, card version 3; sending by the list follows).
- [ ] Lab: two relays; users registered on different ones message and call each other; the preferred relay stopped mid-run.

## R3 — UI

- [ ] Me → Network → My relays: list, add, remove, mark preferred, each with its state.
- [ ] Contact page: the preferred relay and its reachability; the refresh button's final form.

## R4 — QR locator

- [ ] The locator may carry the preferred relay's origin (MR004).
