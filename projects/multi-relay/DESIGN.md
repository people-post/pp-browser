# Multi-relay: design

**Status:** Proposed (2026-11-12), decided in outline ([MR001](DECISIONS.md#mr001--inboxes-are-keyed-by-account-id)–[MR004](DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)). Pre-release: every change below is a clean cutover ([COMPATIBILITY § No bridges](../../docs/contracts/COMPATIBILITY.md#no-bridges-pre-release)).

## Where it stands

- **An app talks to one relay:**
  - it registers there and polls its inbox there;
  - it looks people up in that relay's directory (several directories only as first-answer failover);
  - it sends to recipients' inboxes there.
- **A `relay:` id is local to the relay that issued it.** Ids are random per relay, the send signature covers them, and nothing records which relay an id belongs to. The app treats every `relay:` id as its own relay's (`PeerRouteBook::ForgetAll` on a backend change).
- **A relay accepts mail only from senders registered on it** (`www` `verifySend`). It does not check that the recipient exists.
- **Finding someone by Account ID already works on any relay:**
  - `GET /api/relay/v1/users/by-account/{account_id}` is public;
  - it returns keys, nickname, icon and device endpoints;
  - the keys prove themselves: the Account ID is BLAKE2b-256 of the account's ML-DSA key, and the KEM key carries the account's binding signature.
- **The contact page's sync button** (`ContactsController::OnSyncRemote`):
  - it asks only our directory, by the contact's `relay:` id;
  - it runs a blocking HTTP call on the UI thread, with a 30 s timeout;
  - it fails for a contact added by QR code, which has no `relay:` id.

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
  - the list, preferred first;
  - add a relay by origin, remove one, mark one preferred;
  - each with its registration and reachability.
  - Changing the list signs a new card, registers on new relays, publishes the card to every relay in the list and to contacts over the control lane, and drops the push registration on removed relays.
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
  - Keys are accepted only when they verify against the Account ID; name and photo come from the newest answer.
  - It runs on a worker, shows for every contact, says what changed and what could not be reached, and shows the preferred relay with its reachability.
- **Our own sends:** the sender signs with its account key. A send to a relay we are registered on goes as a registered sender; to any other relay, as a foreign sender with its key.
- **Grants:** the app issues one to each accepted contact, renews it before it expires, keeps the grants it was given, attaches the matching one to each send, and stops renewing a blocked contact's. A send that gets `payment_required` shows the receiver's price (P001's pay-or-alternatives UI).

## Privacy

- **A relay list tells whoever holds the card which relays the user uses.** Cards go to contacts, to group members and to the relays the user registers on. That is the same audience that already learns the user's devices. The list carries no IP address.
- **A relay learns the Account IDs of the senders that write to its users.** That is already true for its own users; a foreign sender is now visible too.

## Not in scope

- Relay-to-relay forwarding or a registry of relays.
- Choosing relays automatically. The user lists them; the app may suggest the org's relays.
- Search across relays by name. A name search asks the relays the user has listed.
