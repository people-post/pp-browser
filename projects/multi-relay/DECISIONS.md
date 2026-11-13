# Multi-relay: decisions

## MR001 — Inboxes are keyed by Account ID

**Date:** 2026-11-12
**Status:** Accepted (Hongwei, 2026-11-12). Not built.
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
