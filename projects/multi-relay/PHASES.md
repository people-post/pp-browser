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
- [ ] `messenger` records keyed by Account ID.
- [ ] Deploy with the app build from R2 (clean cutover).

## R2 — App: Account ID everywhere, the relay list, several relays

- [ ] `ContactIdKind::RelayUser` and `relay:` ids removed (routes, envelopes, contacts, directory hits).
- [ ] The account card's `relays` field (sign, verify, bounds); a contact stores the newest list (a `profile.db` ladder step).
- [ ] Clients per relay: register, poll (preferred at pace, others slower), push tokens on each.
- [ ] Sending in list order with a per-relay health memory; `recipient_not_registered` fetches the card and retries once; a foreign send carries our key.
- [ ] Contracts promoted: ACCOUNT_CARD, SERVICE_ENDPOINTS, WIRE_SCHEMAS, COMPATIBILITY rows.
- [ ] Lab: two relays; users registered on different ones message and call each other; the preferred relay stopped mid-run.

## R3 — UI

- [ ] Me → Network → My relays: list, add, remove, mark preferred, each with its state.
- [ ] Contact page: the preferred relay and its reachability; the refresh button's final form.

## R4 — QR locator

- [ ] The locator may carry the preferred relay's origin (MR004).
