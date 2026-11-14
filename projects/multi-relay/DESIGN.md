# Multi-relay: design

**Status:** Proposed (2026-11-12), decided in outline ([MR001](DECISIONS.md#mr001--inboxes-are-keyed-by-account-id)–[MR004](DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)). Pre-release: every change below is a clean cutover ([COMPATIBILITY § No bridges](../../docs/contracts/COMPATIBILITY.md#no-bridges-pre-release)).

## Where it stands

What holds now is in the contracts ([SERVICE_ENDPOINTS § Several relays](../../docs/contracts/SERVICE_ENDPOINTS.md#several-relays), [ACCOUNT_CARD](../../docs/contracts/ACCOUNT_CARD.md#account-card)); phase status is in [CURRENT_STATE.md](CURRENT_STATE.md). In short:

- **Inboxes, sends and polls are addressed by Account ID** on every relay; there are no `relay:` ids. A relay takes a foreign sender's send when its key hashes to its Account ID, and holds strangers to the receiver's price (grants for contacts, MR005).
- **The account card lists the account's relays**, preferred first, and its floor (card version 3). A contact keeps the newest list with its card (`KeptRelays`); a contact added by QR code starts with the locator's preferred origin until the card arrives (`ContactRelays`).
- **An app registers and polls on up to four relays of its own** (the preferred one, then `relay.extra_base_urls`), and **sends into the recipient's relays in list order**, with a per-relay health memory and one retry on a stale list.
- **The contact page's refresh button** asks the contact's devices, then its relays in list order, then ours, by Account ID, on a worker; keys are kept only when they verify. The page shows the preferred relay by host with whether it answered at the last refresh.
- **The QR locator carries the preferred relay's origin** (optional, within MR004's bounds) and no card.
- **Not yet:** Me → Network → My relays (our other relays come from config); photos, blobs, files, lockers, stream history, search and the Brief LLM key stay on the preferred relay.

## Model

- **A person is their account.** The Account ID is the same on every relay and needs no relay to be checked.
- **Devices are reached over the mesh;** a relay is a mailbox for when they are offline.
- **A relay is named by its origin:** the HTTPS base URL a client uses, e.g. `https://www.brief.global/api/relay`. A relay does not know its own public URL; the client records the one it registered at.
- **Each account publishes its relay list in its account card** ([MR002](DECISIONS.md#mr002--the-relay-list-lives-in-the-account-card)): the relays it is registered on, preferred first. The card is signed by the account key, so any relay or contact can pass it on without being trusted, and the newest card (`issued_at`) wins, as for the device list.

## Account card: `relays`

```json
"relays": [
  { "origin": "https://relay.example.org/api/relay" },
  { "origin": "https://www.brief.global/api/relay" }
]
```

- Ordered: the first entry is the **preferred** relay. An empty list means the account receives over the mesh only.
- **Bounds** ([MR004](DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)):
  - at most 4 entries;
  - an `origin` is an absolute `https://` URL of at most 128 bytes, with no query or fragment;
  - no duplicates.
- **A card breaking a bound is refused whole**, as for the device list.
- **Covered by the card signature:** a new card sign version. A card without the field is refused, a clean cutover ([ACCOUNT_CARD.md](../../docs/contracts/ACCOUNT_CARD.md)).
- **Size:** a card is about 13 KB plus about 4 KB per device; four relays add at most about 0.6 KB.
- **Where the card goes:** the card exchange over Amp, the contact-card chat message, group join bundles (within `kRosterCardsBudgetBytes`) and relay registration. Each already carries several KB.
- **The QR locator carries no card.** It may add the preferred relay's origin alone (about 40 bytes, [MR004](DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)), so a contact added by QR can reach the user's mailbox before it has the card.

## Relays: inboxes by Account ID

[MR001](DECISIONS.md#mr001--inboxes-are-keyed-by-account-id), [MR003](DECISIONS.md#mr003--cross-relay-delivery-is-an-account-signed-send).

- **Registration:**
  - an account registers on a relay as today: a challenge, signed with the ML-DSA key;
  - the relay keeps one row per Account ID, with the account's keys, its endpoints, and the **account card** it was given;
  - there is no `relay:` id any more: the Account ID is the inbox address;
  - the relay accepts the card only if it verifies and names the same Account ID.
- **Directory:**
  - `GET /users/by-account/{account_id}` returns the row and its stored card, so a stranger learns the account's relays from any relay it is registered on;
  - search by nickname stays per relay;
  - `GET /users/{relay_user_id}` goes away.
- **Sending:**
  - `POST /messages` names `sender_account_id` and `recipient_account_id`;
  - the signed bytes cover both, with a new domain or version;
  - the recipient must be registered on this relay (new check); otherwise the relay answers `recipient_not_registered`, which is not a retryable failure.
- **Who may send:**
  - a sender registered on this relay is checked against its stored key, as now;
  - **a foreign sender** (not registered here) sends its ML-DSA public key with the message. The relay checks that the key hashes to `sender_account_id`, then checks the signature;
  - the relay may cache a verified foreign key by Account ID, so later sends can leave it out.
- **Abuse:** the relay tells contacts from strangers, not "registered here" from "foreign" ([MR005](DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant); below).
- **Receiving:** polling, ack and clear are signed by the recipient's account (as the requester) and address the inbox by Account ID.
- **Push:** devices register their push tokens on every relay the account uses, so any relay holding mail can wake the phone.
- **Not built:** forwarding between relays (federation). A sender posts straight into the recipient's relay.

## Strangers and contacts: the receiver's price

[MR005](DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant), with [pricing P001](../pricing/DECISIONS.md#p001--initiation-floor--media-quote-gates-no-payment-rails-yet).

A relay cannot read a message, and it does not know who a user's contacts are. It needs one signal it can check: whether the sender has a grant from the recipient.

- **The receiver's price is in the account card:** `initiation_floor`, in `pp_credit` minor units, the one price for opening a chat or a call with them (P001). It is signed with the card, so every relay and every sender reads the same price. It replaces the floor sent at directory registration.
- **A contact carries a grant.** When a recipient accepts someone, the recipient's app issues a grant and gives it to the sender over the control lane. "Accepts" means: adds them, accepts their opening offer, or waives it. The grant is signed by the recipient's account key, under domain `pp-browser:relay-grant-v1`, over:
  - `recipient_account_id`;
  - `sender_account_id`;
  - `issued_at`;
  - `expires_at` (30 days, renewed while they stay a contact).

  The sender attaches it to every send into the recipient's relays, and the relay checks it with the recipient's key, which it holds.
  - No contact list is uploaded to any relay. A relay sees only the pairs that send through it, as it does today.
  - Blocking stops renewal. Until the grant expires, the recipient's app drops that sender's mail.
- **A relay treats a send without a grant as a stranger's,** whether the sender is registered here or not:
  - **floor above 0:** the send must carry an opening offer of at least the floor. Until payment rails exist, no offer can be honoured, so the relay refuses it (`payment_required`). That matches P001's outbound block, and spares the recipient the spam altogether.
  - **floor 0:** the send is accepted within a first-contact quota per recipient (a few per stranger, a bounded number per hour for all strangers together). It raises no message alert; a call wake is allowed within the per-pair limit.
- **Safety limits apply to everyone:**
  - per sender account;
  - per client address for senders the relay has not verified before (configurable: carrier-grade NAT puts many phones behind one address);
  - the mailbox caps.

## App

- **My relays** (Me → Network):
  - the list, preferred first, kept in the profile (seeded once from the deployment's relay and `relay.extra_base_urls`);
  - add a relay by origin (it must answer), remove one (not the preferred), mark one preferred;
  - each with its registration (kept across restarts) and reachability (its last poll);
  - only the main device changes it; a linked device follows the card's list.
  - Changing the list signs a new card, registers on new relays, publishes the card to every relay in the list and to contacts over the control lane, and revokes the registration (with its push bindings) and drops the inbox cursor on removed relays.
- **Relay services and org services** ([MR006](DECISIONS.md#mr006--the-preferred-relay-carries-the-users-relay-services-org-services-stay-on-the-orgs-origin)):
  - the preferred relay carries the inbox, push, registration, the directory (first), profile photos and blobs, chat attachment files;
  - the org's origin keeps the Brief LLM gateway and its key, Support, diagnostics, crash reports, client compatibility;
  - account lockers and stream history stay on the deployment's relay (no migration yet).
- **Registering and receiving:**
  - the app registers on each listed relay;
  - it polls the preferred relay at today's pace and the others more slowly (e.g. a quarter of that);
  - it drops duplicates by message id (delivery already dedupes).
- **Sending:**
  1. over the mesh when a device of the recipient is online (as today);
  2. else to the recipient's relays in list order: the preferred first, the next on a transport failure or a 5xx.
  - `recipient_not_registered` means our copy of their list is stale: fetch the card and try again once.
  - A per-relay health memory skips a relay that just failed, as the circuit client does for relays.
- **Contacts:**
  - a contact stores the relay list from its newest verified card (a ladder step in `profile.db`);
  - a contact added by QR starts with the locator's preferred origin, if any.
- **The refresh button** asks, in order of trust:
  1. the contact's devices over the mesh (the card exchange);
  2. the contact's relays in list order, by Account ID;
  3. our own relays.
  - The relays are asked up to the first that keeps the account (a relay down, or one without the account, gives way to the next); one asked as the contact's is not asked again as ours.
  - Keys are accepted only when they verify against the Account ID (the relay's stored card and its row's keys alike); name and photo come from the newest answer: the newest kept card's name when a device or a relay's card brought one, else the relay row's; the photo is the row's.
  - It runs on a worker, shows for every contact, says what changed, or that nothing could be reached or found (the plain *card* wording; no source named), and shows the preferred relay with its reachability: what that relay answered at the last refresh (in memory; "not known yet" before one asked it), so no probe runs on its own.
- **Our own sends:** the sender signs with its account key. A send to a relay we are registered on goes as a registered sender; to any other relay, as a foreign sender with its key.
- **Grants:** the app issues one to each accepted contact, renews it before it expires, keeps the grants it was given, attaches the matching one to each send, and stops renewing a blocked contact's. A send that gets `payment_required` shows the receiver's price (P001's pay-or-alternatives UI).

## Privacy

- **A relay list tells whoever holds the card which relays the user uses.** Cards go to contacts, to group members and to the relays the user registers on. That is the same audience that already learns the user's devices. The list carries no IP address.
- **A relay learns the Account IDs of the senders that write to its users.** That is already true for its own users; a foreign sender is now visible too.

## Not in scope

- Relay-to-relay forwarding or a registry of relays.
- Choosing relays automatically. The user lists them; the app may suggest the org's relays.
- Search across relays by name. A name search asks the relays the user has listed.
