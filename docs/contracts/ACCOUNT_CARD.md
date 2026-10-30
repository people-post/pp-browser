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
  "card_version": 2,
  "account_id": "account:…",
  "signing_public_key_b64": "…",
  "kem_public_key_b64": "…",
  "kem_binding_signature_b64": "…",
  "nickname": "alice",
  "devices": [
    { "peer_id": "Qm…", "signing_public_key_b64": "…", "kem_public_key_b64": "…" },
    { "peer_id": "Qm…", "signing_public_key_b64": "…", "kem_public_key_b64": "…" }
  ],
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
- `issued_at` — ms since epoch; the list's version. A device issues a new card only when what it says
  changes (a device joins, a device's keys, the nickname), always later than the newest it knows.
- No addresses: multiaddrs go stale and disclose IPs ([privacy](../../projects/privacy/DECISIONS.md));
  devices are reached through the mesh (below).

**Card signature:** ML-DSA-65 with the account key over: domain `pp-browser:account-card-v2` + NUL,
version u8 `2`, the raw account ML-DSA-65 key, the raw ML-KEM-768 key, `nickname` LenUtf8, device count
u64 BE, then for each device in `peer_id` byte order: `peer_id` LenUtf8, the raw device ML-DSA-65 key, the
raw device ML-KEM-768 key; then `issued_at` i64 BE (LenUtf8 / u64 BE as
in [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088)).

An account card is valid when its key card is valid, `card_signature_b64` verifies with the signing key,
`devices` holds 1–5 entries with no duplicate PeerId, every entry's keys decode to their sizes and its
`peer_id` is the one its device ML-DSA key derives, and `issued_at` is not more than 5 minutes ahead of the
receiver's clock. Version 1 cards (PeerIds only) are refused.

**Newest wins:** the newest valid card (`issued_at`) is the account's word on its keys, nickname and
device list; an older card changes nothing. A restored install keeps no cards but knows each account's newest
issue time from the account locker (`card_floors`, [ACCOUNT_LOCKER](ACCOUNT_LOCKER.md#state-json))
and refuses older cards. Until only a master device issues cards (M021 d4) every
linked device holds the account key, so two may issue at once: a device that learns a card of its own
account listing fewer devices than it knows re-issues with both, and takes the newest card's nickname as
the account's. A newly linked device gets the linking device's card in the link bundle and signs itself
in. Revoking a device is a newer list without it, from d4.

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
| Relay directory | key card fields (+ endpoints) | when a directory is configured ([SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md)) |
| Envelope | sender's account card, `body.e2e.sender_card` | when a copy carries a `key_init` ([MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md#master-psk)) or is an account copy (the control lane, a first contact): whoever reads it can check the sending device; a new device's first message to anyone brings its account's newer list; a restored device sends it to every contact (`device_list` notice) |
| Link bundle | the linking device's account card | linking a device: the new one signs itself in (M021) |
| Sibling notice | the account card, on the control lane to our own mailbox (M022) | when a device's list changes (it linked in, a sibling's newer list, the name), and once a start while it has siblings: they take it |
| Contact card message | someone's account card (or key card, when that is all the sharer has), plus the sharer's name for them and their relay route | "Share" on a contact; *Add to contacts* on the card |
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
the key card; about 128 KB of cards at most, the rest keyed as before), each with the owner's relay route
to that member. A receiver keeps a card only for the roster row it came in, once it proves itself, and
stores the event without the cards; so does the owner's copy (`SendRelayOptions::stored_payload_json`). The invite carries none: an invitee who declines learns no one's keys.

## Storing cards

- Verified cards are kept per account in a persisted card store under the profile vault; the signing
  and KEM key stores are filled from it at unlock (`PeerSigningKeyStore`, `PeerKemKeyStore` refuse
  anything that does not prove itself).
- A contact keeps its account's card; a stranger's card (from a first message) is kept with the thread.

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
