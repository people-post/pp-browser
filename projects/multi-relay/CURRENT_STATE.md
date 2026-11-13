# Multi-relay — current state

**As of:** 2026-11-12

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

**R1 (relays):** on `www` / `messenger` branch `feat/inbox-by-account`, not merged or deployed.

**R2 step 1 (app), built on `feat/account-inbox`, not merged:** one relay as before, addressed by Account ID
everywhere (no `relay:` ids, relay API sign version 2); grants (MR005) issued, delivered over the control lane,
renewed and attached; the relay's new errors handled. Contracts: [SERVICE_ENDPOINTS](../../docs/contracts/SERVICE_ENDPOINTS.md),
[ACCOUNT_CARD § Relay grants](../../docs/contracts/ACCOUNT_CARD.md#relay-grants), [WIRE_SCHEMAS](../../docs/contracts/WIRE_SCHEMAS.md),
[DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md), [COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md#clean-cutovers-pre-release).
Needs pp-cpp-mesh 8.0.0 (directory rows by Account ID; local branch `feat/account-ids`, not tagged).
Calls grant their people (answering grants the caller; joining grants each pair both ways; not renewed for
non-contacts). Not yet: the account card's `relays` and `initiation_floor`; clients per relay; the card at
registration.

Next: R1 and R2 step 1 merge and deploy together (clean cutover), then R2's relay list.
