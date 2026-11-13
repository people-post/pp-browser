# Multi-relay: decisions

## MR001 — Inboxes are keyed by Account ID

**Date:** 2026-11-12
**Status:** Accepted (Hongwei, 2026-11-12). Relays: R1 on `www` / `messenger` `feat/inbox-by-account`; app: R2 step 1 (unmerged). Shipped form: [SERVICE_ENDPOINTS](../../docs/contracts/SERVICE_ENDPOINTS.md).
**Decision:** Relays address inboxes, senders and polls by Account ID. The per-relay `relay:` id goes away, in the relay and in the app (`ContactIdKind::RelayUser`, routes, envelopes).
**Rationale:**
- A `relay:` id is random per relay and means nothing elsewhere, so a user on several relays would have one per relay, and every contact would need a map from relay to id.
- The Account ID is the same everywhere and proves the keys by itself.
- Pre-release this is a clean cutover.
**Alternatives rejected:** keep `relay:` ids and store an origin beside each (the mapping problem stays, multiplied by relays).

## MR002 — The relay list lives in the account card

**Date:** 2026-11-12
**Status:** Accepted (Hongwei, 2026-11-12). Not built.
**Decision:** An account's relays, preferred first, are a field of its signed account card (`relays`). The newest card wins, as for its devices.
**Rationale:**
- The card is already signed by the account key, versioned by `issued_at`, and carried everywhere a contact needs it: the card exchange, contact cards, group bundles and the directory.
- So the list needs no new record, no new signature and no trust in whoever passes it on.
**Alternatives rejected:** a separate signed relay record served by relays (a second record to version and fetch, beside the card that already travels).

## MR003 — Cross-relay delivery is an account-signed send

**Date:** 2026-11-12
**Status:** Accepted (Hongwei, 2026-11-12). Not built.
**Decision:**
- A sender posts straight into the recipient's relay.
- A relay accepts a sender that is not registered on it when the message carries the sender's ML-DSA key, the key hashes to the sender's Account ID, and the signature verifies.
- The recipient must be registered on that relay.
- Relays do not forward to each other.
**Rationale:** It needs no trust between relays, no relay identities and no registry of relays, only a check each relay can do alone. Abuse is bounded per sender account, per IP and by mailbox caps.
**Alternatives rejected:**
- The app registers on every relay it sends to (one registration per relay per contact's relay, and senders that look unregistered).
- Federation between relays (relay identities, a peer list, trust between operators: a much larger design).

## MR004 — A relay list is small and bounded

**Date:** 2026-11-12
**Status:** Accepted (Hongwei asked whether the card could overflow, 2026-11-12). Not built.
**Decision:**
- At most 4 relays.
- Each origin is an absolute `https://` URL of at most 128 bytes, with no query or fragment.
- No duplicates.
- A card breaking a bound is refused whole.
- The QR locator may carry the preferred relay's origin alone.
**Rationale:**
- A card is about 13 KB plus about 4 KB per device; four relays add at most about 0.6 KB, so no path that carries a card comes near its limit.
- The QR code is the only tight one: it carries no card, and one origin of about 40 bytes keeps it readable.

## MR005 — Strangers pay the receiver's price; contacts carry a grant

**Date:** 2026-11-12
**Status:** Accepted (Hongwei, 2026-11-12: "factor in our design in pricing project … more effective and efficient"). Grants built (R2 step 1, unmerged; shipped form [ACCOUNT_CARD § Relay grants](../../docs/contracts/ACCOUNT_CARD.md#relay-grants)); the floor in the card not yet. Amends [pricing P001](../pricing/DECISIONS.md#p001--initiation-floor--media-quote-gates-no-payment-rails-yet) item 7: the floor is published in the account card, not at directory registration.
**Decision:**
- The receiver's `initiation_floor` is a field of the signed account card.
- A recipient's app gives each accepted contact a grant: a signed statement that this sender may write to it, expiring in 30 days and renewed while they stay a contact. The sender attaches it to its sends, and a relay checks it with the recipient's key.
- *Amended 2026-11-13 (the app lab's `call_grows_into_a_group`, Hongwei's session):* a call accepts its people too. Answering a call grants the caller; joining a call, or someone joining a call we are in, grants each pair of participants both ways. These grants are not renewed unless the peer is also a contact or group member, and a blocked account gets none. A call's signaling, and a call grown past contacts, would otherwise be held to the stranger quota.
- **A send without a grant is a stranger's.** With a floor above 0, it must carry an opening offer of at least the floor; until payment rails exist, the relay refuses it with `payment_required`. With a floor of 0, it gets a small first-contact quota per recipient and raises no message alert.
- **Safety limits apply to everyone:** per sender account, per client address for unverified senders (configurable), and the mailbox caps.
**Rationale:**
- Rate limits only bound the damage. A price on first contact removes the incentive, and the receiver chooses it, as pricing P001 already intends.
- The relay cannot read mail or see contact lists. A grant gives it the one fact it needs, checked with a key it already holds, without uploading anyone's contacts.
- "Registered here or foreign" then stops mattering for abuse.
**Alternatives rejected:**
- Rate limits alone (a new account costs nothing).
- Uploading each user's allow-list to their relays (the relay learns the contact graph).
- Treating foreign senders as suspect and local ones as trusted (abuse does not depend on where the sender registered).
