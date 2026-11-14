# Multi-relay: decisions

## MR001 — Inboxes are keyed by Account ID

**Date:** 2026-11-12
**Status:** Accepted (Hongwei, 2026-11-12). Relays: R1 (`www` #252); app: R2 step 1 (#609). Shipped form: [SERVICE_ENDPOINTS](../../docs/contracts/SERVICE_ENDPOINTS.md).
**Decision:** Relays address inboxes, senders and polls by Account ID. The per-relay `relay:` id goes away, in the relay and in the app (`ContactIdKind::RelayUser`, routes, envelopes).
**Rationale:**
- A `relay:` id is random per relay and means nothing elsewhere, so a user on several relays would have one per relay, and every contact would need a map from relay to id.
- The Account ID is the same everywhere and proves the keys by itself.
- Pre-release this is a clean cutover.
**Alternatives rejected:** keep `relay:` ids and store an origin beside each (the mapping problem stays, multiplied by relays).

## MR002 — The relay list lives in the account card

**Date:** 2026-11-12
**Status:** Accepted (Hongwei, 2026-11-12). The field built (R2 step 2: card version 3, kept per contact; the relay stores and serves it); sending by it built (R2 step 3). Shipped form: [ACCOUNT_CARD § Account card](../../docs/contracts/ACCOUNT_CARD.md#account-card), [SERVICE_ENDPOINTS § Sending by the list](../../docs/contracts/SERVICE_ENDPOINTS.md#sending-by-the-list).
**Decision:** An account's relays, preferred first, are a field of its signed account card (`relays`). The newest card wins, as for its devices.
**Rationale:**
- The card is already signed by the account key, versioned by `issued_at`, and carried everywhere a contact needs it: the card exchange, contact cards, group bundles and the directory.
- So the list needs no new record, no new signature and no trust in whoever passes it on.
**Alternatives rejected:** a separate signed relay record served by relays (a second record to version and fetch, beside the card that already travels).

## MR003 — Cross-relay delivery is an account-signed send

**Date:** 2026-11-12
**Status:** Accepted (Hongwei, 2026-11-12). Built: the relay takes a foreign sender's key (R1, `www` #252), the app sends into a recipient's relays, as a foreign sender where it is not registered (R2 step 3). Shipped form: [SERVICE_ENDPOINTS § A send](../../docs/contracts/SERVICE_ENDPOINTS.md#a-send-the-recipient-the-senders-key-the-grant), [§ Sending by the list](../../docs/contracts/SERVICE_ENDPOINTS.md#sending-by-the-list).
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
**Status:** Accepted (Hongwei asked whether the card could overflow, 2026-11-12). The card's bounds built (R2 step 2, app and relay alike, [ACCOUNT_CARD § Account card](../../docs/contracts/ACCOUNT_CARD.md#account-card)); our own relays bounded the same (at most 4, R2 step 3, [SERVICE_ENDPOINTS § Several relays](../../docs/contracts/SERVICE_ENDPOINTS.md#several-relays)); the QR locator's origin built (R4: an optional `relay`, the card's bounds, [ACCOUNT_CARD § Locator](../../docs/contracts/ACCOUNT_CARD.md#locator-qr-code-invite-link)).
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
**Status:** Accepted (Hongwei, 2026-11-12: "factor in our design in pricing project … more effective and efficient"). Grants built (R2 step 1; shipped form [ACCOUNT_CARD § Relay grants](../../docs/contracts/ACCOUNT_CARD.md#relay-grants)); the floor in the card built (R2 step 2: card version 3, sent at registration, read by the relay for strangers — [SERVICE_ENDPOINTS](../../docs/contracts/SERVICE_ENDPOINTS.md)). Amends [pricing P001](../pricing/DECISIONS.md#p001--initiation-floor--media-quote-gates-no-payment-rails-yet) item 7: the floor is published in the account card, not at directory registration.
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

## MR006 — The preferred relay carries the user's relay services; org services stay on the org's origin

**Date:** 2026-11-14
**Status:** Accepted (Hongwei, 2026-11-14: "relay services follow the preferred relay; org services stay on the org's address"). Built with R3's *My relays* (Me → Network); shipped form [SERVICE_ENDPOINTS § Relay services and org services](../../docs/contracts/SERVICE_ENDPOINTS.md#relay-services-and-org-services). Amended 2026-11-14 (review): a release build takes no relay at a local address, from Add or a QR locator ([SERVICE_ENDPOINTS § My relays](../../docs/contracts/SERVICE_ENDPOINTS.md#my-relays)); removing the deployment's relay does not stop the locker writes (they fail there, logged) and moves stream history's reads to the preferred relay, as built.
**Decision:**
- **Two origins.** The **org's origin** is the deployment's: the compiled (or configured) backend origin, `relay` / `registration` in the config file. The **preferred relay** is the first of the user's own relay list (*My relays*, kept in the profile). Until the user changes the list the two are the same, and nothing behaves differently.
- **Relay services follow the preferred relay** (and the list): the inbox (polled at the app's pace; the others more slowly), push, registration (the identity's), the directory (search and lookups: the preferred relay's first, then the others'), profile photos and blobs, chat attachment files.
- **Org services stay on the org's origin, whichever relay is preferred:** the Brief LLM gateway and its key, Support (client compatibility's `support`), diagnostics, crash reports, client compatibility, the promoted MCP server. The Brief LLM key is the org's: only a registration at the org's relay gives one; another relay's answer is not kept.
- **What stays where it is (no migration in this step):** account lockers and stream history stay on the deployment's relay, the relay that holds them: every locker and stream so far was written there, and a restoring device, which knows no list yet, looks there. Removing that relay from the list revokes our registration there: the locker writes still go there and fail (logged; the relay takes them from a registered account only), the restore's read (by locker id) still works, and stream history is asked of the preferred relay instead.
- **A preferred change** re-points the relay services, renews the identity's registration on the new preferred relay (the card is newer, so it goes whatever the auto-renew preference — the path #602 built for a backend switch, so the registration records the relay it was made at), and registers on the old one as on any other relay of ours.
- **The list** is user data in the profile, seeded once from the deployment's relay and `relay.extra_base_urls`; afterwards config is only the default for a new profile. At most 4 (MR004), `https://` (an `http://` relay, or one on this device or a private network, only in developer builds, and never on the card; a QR locator's relay alike), not empty in this step (mesh-only is not offered), the preferred one not removable until another is preferred. Only the main device changes it (it signs the card's list, MR002); a linked device follows the newest card's list.
- **Every change** re-signs our card with the new list (preferred first), registers on new relays, publishes the card to every listed relay (each registers again for a newer card) and to our contacts and siblings over the control lane (the device-list notice with the card), and polling and sending follow the new list. A relay added must answer `GET /v1/client-compat`; a relay removed is revoked there (its push bindings with it) and its inbox cursor dropped.
**Rationale:**
- The org runs services that are not a mailbox: its LLM gateway, its Support desk, its crash and diagnostics intake and its release policy. They belong to the org that built and ships the app, not to whichever relay a user prefers, and a third-party relay need not run them at all.
- Mailbox services are what a relay is for: a user who prefers another relay wants their mail, photos and files there, and the directory they search first to be the one they chose.
- Moving lockers and stream history needs a migration (re-sealing, rewriting on the new relay); a restore must still find them. Leaving them on the deployment's relay keeps both until a later step.
**Alternatives rejected:**
- Everything follows the preferred relay (an independent relay would have to run the org's LLM gateway and Support, and the org's key would be issued by someone else).
- Everything stays on the org's origin except the inbox (the user's photos, files and directory would stay with a relay they moved away from).
