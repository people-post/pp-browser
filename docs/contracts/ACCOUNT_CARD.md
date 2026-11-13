# Account cards: keys and devices that prove themselves

**Tier:** contract

An **account card** says which keys and devices belong to an Account ID. It is signed by the account
itself, so whoever carries it — the relay directory, the peer, a friend forwarding a contact, a QR
code — needs no trust: the receiver checks it against the Account ID alone. Cards are how a device
encrypts to and verifies a person it already knows **without the relay** ([E028](../../projects/e2e-message-crypto/DECISIONS.md#e028--account-cards-keys-that-prove-themselves-relay-optional)).

Finding someone new by name is not this contract's job: the relay directory answers it now and the
chain name registry later ([N029](../../projects/p2p-mesh/NAME_DIRECTORY_NORTH_STAR.md)); without
either, a person is added from a card or invite handed over out of band.

**Related:** [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md) (where the keys are used),
[SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md) (the directory returns the same keys),
[P2P_MESSAGING.md](../architecture/P2P_MESSAGING.md#finding-and-reaching-people) (discovery and reach).

## Identity recap

| Key | Holder | Proves |
|-----|--------|--------|
| Account ML-DSA-65 (signing) | every linked device | the Account ID: `account:<base64url-unpadded(BLAKE2b-256(pk))>` |
| Account ML-KEM-768 | every linked device | nothing by itself — bound by the **KEM binding** |
| Device ML-DSA-65 | one device | its PeerId (mesh identity); listed on the account card |
| Device ML-KEM-768 | one device | nothing by itself — listed on the account card ([M021](../../projects/multi-device-account/DECISIONS.md#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits)); never leaves the device |

## Key card

The keys a sender needs to verify an account's envelopes and to encrypt to it.

```json
{
  "account_id": "account:…",
  "signing_public_key_b64": "…",
  "kem_public_key_b64": "…",
  "kem_binding_signature_b64": "…"
}
```

A key card is valid when:

1. `signing_public_key_b64` decodes to a 1952-byte ML-DSA-65 key that hashes to `account_id`;
2. `kem_binding_signature_b64` verifies with that key over the **KEM binding** bytes
   (`foundation/crypto/KemKeyBinding.h`): domain `pp-browser:kem-binding-v1` + NUL, version u8 `1`,
   the raw ML-DSA-65 account key, the raw 1184-byte ML-KEM-768 key.

The relay directory returns exactly these four fields with every entry, so a directory answer and a
card are checked the same way.

## Account card

A key card plus what the account says about itself, signed as a whole.

```json
{
  "card_version": 3,
  "account_id": "account:…",
  "signing_public_key_b64": "…",
  "kem_public_key_b64": "…",
  "kem_binding_signature_b64": "…",
  "nickname": "alice",
  "devices": [
    { "peer_id": "Qm…", "signing_public_key_b64": "…", "kem_public_key_b64": "…" },
    { "peer_id": "Qm…", "signing_public_key_b64": "…", "kem_public_key_b64": "…" }
  ],
  "relays": [
    { "origin": "https://www.brief.global/api/relay" }
  ],
  "initiation_floor": 0,
  "issued_at": 1761235200000,
  "card_signature_b64": "…"
}
```

- `nickname` — the account's own name for itself: a hint, shown as such; never an identity
  ([N029](../../projects/p2p-mesh/NAME_DIRECTORY_NORTH_STAR.md): no second name system).
- `devices` — **the account's device list** (M021), the issuing device first, at least 1 and at most 5:
  each device's PeerId, the device ML-DSA-65 key that PeerId is derived from, and the device's own
  ML-KEM-768 key. This is the account's standing statement that those devices speak for it; nothing else
  in the system makes it.
- `relays` — **the relays the account receives on**, preferred first ([multi-relay MR002](../../projects/multi-relay/DECISIONS.md#mr002--the-relay-list-lives-in-the-account-card)):
  each named by its origin, the HTTPS base URL a client uses. Required; an empty list means the account
  receives over the mesh only. Bounds ([MR004](../../projects/multi-relay/DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)):
  at most 4 entries; each `origin` an absolute `https://` URL of at most 128 bytes with a host, and no query,
  fragment, credentials, spaces or control characters; no two the same relay (scheme and host compared without
  case, `:443` as no port, an empty path as `/`). Today the list holds the one relay the app registers on
  (`OwnCards::SetRelays`, the registration base URL); an origin outside the bounds — an `http://` lab or dev
  relay — is left off, so such a card says the mesh only. The list is the **main device's**: the others keep the
  newest card's.
- `initiation_floor` — the account's price for opening a chat or a call with it, in `pp_credit` minor units
  ([pricing P001](../../projects/pricing/DECISIONS.md#p001--initiation-floor--media-quote-gates-no-payment-rails-yet),
  [MR005](../../projects/multi-relay/DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant)):
  required, an integer from 0 to 2^53 − 1 (the relay reads it as a JSON number). Every relay reads a stranger's price
  from it; the identity's `initiation_floor` (dogfood: `AppConfig.initiation_floor`) is the account's, and a device
  that takes a sibling's newer card takes its floor, as its nickname.
- `issued_at` — ms since epoch; the card's version. A device issues a new card only when what it says
  changes (a device joins, a device's keys, the nickname, the relays, the floor), always later than the newest it
  knows.
- No addresses: multiaddrs go stale and disclose IPs ([privacy](../../projects/privacy/DECISIONS.md));
  devices are reached through the mesh (below).

**Card signature:** ML-DSA-65 with the account key over: domain `pp-browser:account-card-v3` + NUL,
version u8 `3`, the raw account ML-DSA-65 key, the raw ML-KEM-768 key, `nickname` LenUtf8, device count
u64 BE, then for each device in `peer_id` byte order: `peer_id` LenUtf8, the raw device ML-DSA-65 key, the
raw device ML-KEM-768 key; then relay count u64 BE and each relay's `origin` LenUtf8 **in list order** (the
preferred relay is signed as the first); then `initiation_floor` i64 BE and `issued_at` i64 BE (LenUtf8 / u64 BE as
in [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088)). Golden vector (SHA-256 of the bytes, 9674 of
them): `AccountCardSignBytesTest.MatchTheGoldenVector` in `account_card_test.cpp`, the same in the relay's
`AccountCard.test.ts`.

An account card is valid when its key card is valid, `card_signature_b64` verifies with the signing key,
`devices` holds 1–5 entries with no duplicate PeerId, every entry's keys decode to their sizes and its
`peer_id` is the one its device ML-DSA key derives, `relays` and `initiation_floor` are present and within their
bounds, and `issued_at` is not more than 5 minutes ahead of the receiver's clock. A card breaking any bound is
refused whole, by the app and the relay alike. Cards of any other `card_version` (1: PeerIds only; 2: no relays or
floor) are refused.

**Newest wins:** the newest valid card (`issued_at`) is the account's word on its keys, nickname,
device list, relays and floor; an older card changes nothing. A restored install keeps no cards but knows each account's newest
issue time from the account locker (`card_floors`, [ACCOUNT_LOCKER](ACCOUNT_LOCKER.md#state-json))
and refuses older cards. A device takes the newest card's nickname as the account's.

**Main device (master, [M021](../../projects/multi-device-account/DECISIONS.md#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) d4):** the first device on the list. Only it changes the list: takes a device off
(revoke: a newer list without it) or hands the role on (another device first). Any listed device re-issues the
card to bring its own entry or the nickname up to date, the list otherwise as it is. Only the main device gives
out a transfer code; the new device gets its card in the link bundle and appends itself at the end, once
(`card_intent` join). While a code is out (15 min), the main device keeps a device that only an older card of
the account lists — a join that reached it after it issued another card. When the main device takes in the card a
new device appended itself to, it sends that card to its contacts (the `device_list` notice), so their next
message reaches the new device too. A device restored from the recovery
phrase starts a fresh list holding only itself (`card_intent` fresh), newer than any it or the account's card
floor knew. A device the newest list leaves out is **signed out**: it issues no card, sends nothing, and stops
syncing the account locker; Me → Security says so. The rule is followed by the account's own devices — each
still holds the account key, so a stolen device could sign itself back in (M021 limit 4).

**Size:** a key card is about 13 KB as JSON (two 3309-byte signatures and two public keys in base64);
each device adds about 4 KB to an account card. Too large for a QR code, which is why invites carry a
locator.

## Locator (QR code, invite link)

What fits in a QR code: who to look for and where.

```
pp-browser-contact-v1:<base64url-unpadded(JSON)>
{ "account_id": "account:…", "nickname": "alice", "devices": ["Qm…", "Qm…"] }
```

Unsigned: the Account ID is what the person showed or sent. The receiver fetches the account's card
from a listed device ([card exchange](#card-exchange-over-amp)) or, when one is configured, from the
relay directory, and accepts it only for that `account_id`. Listed devices are hints; the card decides.

- **Sharing:** the invite text (*Me → Profile → Invite*, and the share at the end of onboarding) ends
  with the locator line, so pasting the whole message works.
- **Adding:** *Add contact* finds a locator anywhere in pasted text (`FindContactLocator`). The person is
  added at once under the locator's nickname; the card is fetched from the listed devices (each reached
  through the mesh if no link is up), then the directory. With neither answering the contact stays and
  the user is told they can write once a device is reachable; keys are then resolved as below.
- **QR code:** *Me → Profile → QR code* shows the locator as a QR code on Mac, iOS and Android
  (`lib/ui/QrSvg`, vendored qrcodegen; dark on white whatever the theme).
- **Link:** the locator is itself a URI, and the app registers its scheme `pp-browser-contact-v1` (iOS /
  macOS `CFBundleURLTypes`, an Android `VIEW` intent filter; a desktop launcher passes it as an argument).
  So a phone's camera scanning the QR code, or a tap on the line in another app, opens the app with it
  (`IncomingLink`): once unlocked it asks *Add {name} to your contacts?* and then adds them as a pasted
  invite. A link never adds anyone unasked.
- **In-app scan:** *Scan QR code* beside Me reads the same locator and offers the same explicit
  confirmation before adding. The scanning surface returns decoded text through caller-owned ports;
  it does not execute arbitrary QR contents. See [SCANNING.md](../ui/SCANNING.md).

## Where cards travel

| Carrier | What | When |
|---------|------|------|
| Relay directory | key card fields (+ endpoints), and the account card it registered with (`GET /users/by-account`) | when a directory is configured ([SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md)) |
| Relay registration | our account card (`register/finish` `account_card`) | every registration and renewal, and again when our card is newer than the one the relay last answered for; the relay reads our floor from it |
| Envelope | sender's account card, `body.e2e.sender_card` | when a copy carries a `key_init` ([MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md#master-psk)), so a new device's first message to anyone brings its account's newer list. On the control lane (a copy per device of the peer's, or the account copy — M024), only when a device it addresses may not hold it (`ControlLaneCarriesCard`, told per device of the peer's: a device copy's device, every device on the peer's card for an account copy): the first lane record this device sends each such device after the card changed (or since it started — a device just linked to the peer's account has been told nothing, whatever its siblings were), once a record, and always a `device_list` notice (a restored device sends it to every contact), a card or copy ask, a group's member check (the asker may be a restored device), a ringing call invite. The card is ~13 KB + 4 KB per device: it used to ride every lane message, and the relay filled with them. A receiver that cannot check an envelope — no card kept, or the sending device not on it — asks that device for the card (`card_ask`, at most once per account per 10 min and 8 a minute in all: the envelope is unverified, and its `sender_card_at` counts for nothing — [WIRE_SCHEMAS § card_ask](WIRE_SCHEMAS.md)); a lane record waits for it in memory (≤ 2 min) and is checked again once a card of the sender's is kept ([P2P_MESSAGING § The control lane](../architecture/P2P_MESSAGING.md#the-control-lane)). A device that finds a verified lane record with no copy for it, or an account copy it cannot open, sends the sender its own card (`device_list`, at most once per account per 10 min) |
| Envelope hint | how new the sender's list is, `body.e2e.sender_card_at` | on every envelope (M021 d6): a receiver holding an older card asks the sending device for the newer one (`card_ask` → a `device_list` notice; unsigned, so it never stops a later ask — `CardAskGate`), so a device taken out stops getting copies from peers the main device's announcement did not reach (a group member who is not a contact) |
| Link bundle | the linking device's account card | linking a device: the new one signs itself in (M021) |
| Sibling notice | the account card, on the control lane to our own mailbox (M022) | when a device's list changes (it linked in, a sibling's newer list, the name), and once a start while it has siblings: they take it |
| Contact card message | someone's account card (or key card, when that is all the sharer has), plus the sharer's name for them | "Share" on a contact; *Add to contacts* on the card |
| Locator | account id, nickname, devices | QR code, invite link, onboarding share |
| Card exchange over Amp | an account card or key card | on request, device to device |
| Roster add (`member_joined`) | members' cards | from the group owner, so members key pairwise ([Groups](#groups)) |

## Card exchange over Amp

App protocol `/pp/account-card/1.0.0` (registered through `MeshHost::SetAppProtocols`), one JSON
request and reply per stream:

```json
→ { "op": "get_card", "account_id": "account:…", "auth": { … } }
← { "card": { …account card… } }   |   { "key_card": { …key card… } }   |   { "error": "not_available" }
```

- Any device answers for its own account only.
- Answering at all tells the requester that this device belongs to the account it asked about, so a
  device answers only requesters that say who they are: `auth` is a `PeerRequestAuth` (the requester's
  account signature over its device PeerId and a timestamp, bound to this Amp link). Authenticated
  requesters that are not blocked get the **account card**; unauthenticated requests get nothing, unless
  this account's address audience is *Everyone*, when they get the **key card** (the keys the directory
  publishes, no devices). A blocked account and an unauthenticated one get the same `not_available`.
- A device appears in the card it serves; a card that does not list the device asked about is ignored.
- The request carries the requester's key card (`auth.requester_keys`), so a device checks a stranger with
  no lookup; it is kept once the request verifies.

**When a device asks.** A call names each side's device (its signed invite / accept), and a verified
message can arrive over a link with a device: when the account's kept card does not list that device yet,
this device asks it for the card — over its own link first, then the account's dial key — and the card,
not the call or the message, ties the device to the account. A device that answered is not asked again
for 10 minutes; one not reached (its link still coming up) is tried again after 15 s, up to 3 times.

**Refresh on the contact page.** The user's *Refresh account info* asks the contact's devices (the kept
card's, then the contact's device rows) for the account's card even when one is kept
(`ContactCardExchange::AskDevices`; at most once per account per 15 s, a press within that gets the last
ask's outcome), then the relay directory by Account ID (a contact with no Account ID asks no one: the
directory knows people by it alone, multi-relay MR001). Keys are kept only as above: the card or the directory's key card must verify against the Account
ID; a directory answer whose keys do not is used for its name and photo only, and one for another account
not at all; the user is told either way. Name: the card's when a device answered, else the directory's;
photo and device addresses: the directory's ([`ContactRefreshWorkflow`](../../capabilities/messaging/src/feature/conversations/ContactRefreshWorkflow.h)).

**Using the devices.** A send to an account goes over a live link to any device its kept card lists
(`AmpDirectChatTransport::ReachableKey`); the contact's endpoint rows follow the card, so dials to those
devices pass the address audience.

## Reaching an idle device

With a PeerId and no address, the mesh reaches a device through a relay circuit on a seed it is
parked on. Between calls nothing keeps a device parked, so:

- **Phones** are woken through the relay's push (the platforms' push services); relay-free reach covers
  them while they are online.
- **Desktops** can stay reachable with no relay: *Me → Network → Reachable without the relay*
  (`mesh.reachable_without_relay`, off by default) renews a reservation on the seeds every 20 s. The seeds
  see when the computer is online.

## Groups

Members encrypt to and verify each other pairwise, and two members may never have met. The owner — who
has every member's card from their invite chats — gives them out with the roster (`member_joined`,
[group chat DESIGN](../../projects/group-chat/DESIGN.md#membership-event-wire-schema)): the existing
members get the newcomer's card, the newcomer everyone's (the newest account card the owner keeps, else
the key card; about 128 KB of cards at most, the rest keyed as before). The relay reaches each member by
its Account ID, so no route rides along. A receiver keeps a card only for the roster row it came in, once it proves itself, and
stores the event without the cards; so does the owner's copy (`SendRelayOptions::stored_payload_json`). The invite carries none: an invitee who declines learns no one's keys.

## Relay grants

A relay cannot see contact lists. It takes a send as a **contact's** when it carries a **grant** from
the recipient, else as a **stranger's**: the recipient's price, or a small free quota with no push
([multi-relay MR005](../../projects/multi-relay/DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant)).
The relay's side is [www `docs/relay-messages.md`](https://github.com/people-post/www); the send and
its errors are in [SERVICE_ENDPOINTS § A send](SERVICE_ENDPOINTS.md#a-send-the-recipient-the-senders-key-the-grant).

**The grant** (`RelayGrant`, JSON on the send's `grant` and in the lane notice):

```json
{ "recipient_account_id": "account:bob…", "sender_account_id": "account:alice…",
  "issued_at": 1761235200000, "expires_at": 1763827200000, "signature": "<base64>" }
```

Signed by the **recipient's** account ML-DSA-65 key over

```
"pp-browser:relay-grant-v1" 0x00 | u8 1 | str recipient_account_id | str sender_account_id
| i64be issued_at | i64be expires_at                       (str = u64be byte length ‖ UTF-8)
```

`expires_at − issued_at` is at most **30 days**. The relay checks the signature with the recipient's
registered key; the app checks a grant it receives the same way, with the grantor's key resolved as
[above](#resolving-keys) (`CheckRelayGrant`: the pair, the lifetime, not expired, the key hashes to the
grantor's Account ID). Golden vector: `relay_message_sign_payload_test.cpp` (`RelayGrantSignBytesTest`),
ported from the relay.

**Who gets one.** Every contact not blocked (added; an opening offer accepted or waived adds the contact)
and every member of a group we are in (`RelayGrantPeers`). Calls grant too, contacts or not: **answering a
call** grants the caller and anyone already joined in it, and **someone joining a call we are in** is
granted when it says so itself (its accept to our call, or its own `call_roster` naming it joined while we
are; a roster's word about third parties grants nobody) — so each side of every pair in a call grants the
other (`GrantCallPeers`). A call's signaling is more than a stranger's three sends an hour, and a call grown
past contacts (Alice calls Bob, then adds Carol, whom Bob does not know) needs Bob and Carol to reach each
other. These are ordinary 30-day grants, but only a contact's or a member's is renewed; a blocked account
gets none from a call either. Before an answer, a stranger's invite (and a resend) fits the stranger quota,
and control-lane sends never wait behind chat sends. Our own account needs none: a send to our own mailbox
never carries a grant. Contacts and members we had before the cutover get theirs on the first run (the sweep
finds no grant issued to them).

**How it travels.** Over the control lane, peer to peer (`RelayGrantCoordinator`; family `relay_grant`):

| `control_type` | `detail` | Meaning |
|----------------|----------|---------|
| `relay_grant` | the grant | a grant to the receiver; kept when it checks, newer replaces older |
| `relay_grant_ack` | `{ "issued_at" }` | the grantee kept the grant issued then |
| `relay_grant_ask` | `{}` | the sender holds no usable grant from the receiver |

Lane records older than an hour are dropped, so a grant can be lost while its grantee is offline. An
issued grant stays **unconfirmed** until acked and goes again when its grantee is next heard from (a
verified envelope), at most once an hour; an ask is answered at once (at most every 10 min). An ask is
itself a stranger's send, and the relay allows a stranger few (3 an hour per pair), so a send never asks:
a device asks a peer it grants only when it hears from that peer after lacking its grant for a few minutes
(a peer that just added us sends its grant unasked), at most every six hours, and 10 min after a refusal.
When a grant arrives, the sends to its grantor that the stranger quota held back go at once — control-lane retries first, in the order they were refused, so a call invite still reaches the callee before its own `call_ended`.

**Renewal and stop.** A sweep on messaging ready, on each contacts or roster change and every six hours
issues to each granted peer whose grant has less than **10 days** left. A blocked contact or a removed
member is no longer granted: nothing is renewed, and the grant it holds runs out. A call's grant to someone
who is neither is not renewed: asked for, it is sent again while it lasts, and then it runs out.

**On a send.** The relay client attaches the recipient's grant when it is still usable (more than 10 min
left). When the relay answers `grant_expired` or `invalid_grant`, the grant is dropped and asked for
again, and the send goes **once** as a stranger's meanwhile.

**Kept** in profile.db (`relay_grants`, [DATA_LAYOUT](DATA_LAYOUT.md)): the grants given to us, one per
grantor, and the grants we issued, one per grantee, with whether it was confirmed.

## Storing cards

- Verified cards are kept per account in a persisted card store under the profile vault; the signing
  and KEM key stores are filled from it at unlock (`PeerSigningKeyStore`, `PeerKemKeyStore` refuse
  anything that does not prove itself).
- A contact keeps its account's card; a stranger's card (from a first message) is kept with the thread.
- The relays and floor an account's newest kept card says are read with `KeptRelays` / `KeptInitiationFloor`
  (`PeerCards.h`): none while only a key card is known. Sending by the relay list is multi-relay R2 step 3.
- Our own kept card is re-issued, its device list as it is, when it does not verify (`OwnCards`): the version 3
  step of `profile.db` leaves our version 2 card in the version 3 shape with its old signature ([DATA_LAYOUT § Schema versions](DATA_LAYOUT.md#schema-versions)).

## Resolving keys

For an Account ID, signing and KEM keys come from, in order:

1. the card store;
2. a card fetched from one of the account's known devices over Amp (`get_card`);
3. the relay directory, when one is configured;
4. `[later]` the chain name registry (N029 phase D, [E024](../../projects/e2e-message-crypto/DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007)).

Every source goes through the same checks; none is trusted for more than carrying the bytes.

## Trust

A card proves *these keys and devices belong to this Account ID*. That the Account ID is the person
you think it is rests on how you got it — in person (QR), from them, from a friend's contact card, or
from the directory's search — exactly as with the directory today. The contact screen shows the
account fingerprint for comparing out of band (E016).
