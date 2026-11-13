# Multi-relay — current state

**As of:** 2026-11-12

Design written ([DESIGN.md](DESIGN.md)); MR001–MR005 accepted.

**R0 built** (app only, no wire change): the contact page's *Refresh account info* is on every contact's page
(phone row and desktop button; no longer behind *advanced* or a `relay:` id). It runs
`ContactRefreshWorkflow` (`capabilities/messaging/src/feature/conversations/`): the contact's devices are asked
for the account card even when one is kept (`ContactCardExchange::AskDevices`, at most once per account per
15 s), then our directory by Account ID (by `relay:` id only for a contact with no Account ID) on a worker; the
merge and the write land on the UI thread (`AppRuntime::PostUI`), the button reads "Refreshing…" meanwhile, and
a second press does nothing. Keys are kept only through `AccountKeyResolver` (`VerifyKeyCard` /
`VerifyKemBinding`: the signing key hashes to the Account ID, the KEM key carries its binding); a directory
answer whose keys do not verify is used for name and photo only, one for another account not at all, and the
user is told. The toast says what changed (name, photo, keys, devices), "Already up to date", or which source
could not be reached. Contract text: [ACCOUNT_CARD § Card exchange](../../docs/contracts/ACCOUNT_CARD.md#card-exchange-over-amp).

Not yet in R0's scope (later phases): the contact's own relays (R2's relay list) and our other relays are not
asked, and the page does not show the preferred relay (R3). DESIGN's "Where it stands" still describes the old
button.

Next: R1, relays keyed by Account ID (`www`, `messenger`).
