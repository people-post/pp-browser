# Account cards: keys and devices that prove themselves

**Tier:** contract

An **account card** says which keys and devices belong to an Account ID. It is signed by the account
itself, so whoever carries it — the relay directory, the peer, a friend forwarding a contact, a QR
code — needs no trust: the receiver checks it against the Account ID alone. Cards are how a device
encrypts to and verifies a person it already knows **without the relay** ([E028](../../projects/e2e-message-crypto/DECISIONS.md#e028--account-cards-keys-that-prove-themselves-relay-optional)).

Finding someone new by name is not this contract's job: the relay directory answers it now and the
chain name registry later ([N029](../../projects/p2p-mesh/NAME_DIRECTORY_NORTH_STAR.md)); without
either, a person is added from a card or invite handed over out of band.

**Status:** Card v5, the key card and KEM binding v2 are release 1's forms, from the signed-format cutover
([release-compatibility RC020](../../projects/release-compatibility/DECISIONS.md#rc020--the-signed-formats-for-the-release-1-cutover),
SIGNED_FORMATS § 2.2–2.3; on `cutover/signed-formats` until merge day): typed keys, relays by relay id, an extension
area, the domains without `-vN`. Nothing older is read ([COMPATIBILITY](COMPATIBILITY.md)).

**Related:** [SIGNED_EXTENSIONS.md](SIGNED_EXTENSIONS.md) (versions, typed blobs, the extension area, the receiver's
order), [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md) (where the keys are used),
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
  "kem_binding_b64": "…"
}
```

The keys are **typed blobs** ([SIGNED_EXTENSIONS § Typed blobs](SIGNED_EXTENSIONS.md#typed-blobs)): the algorithm id,
then the key — ML-DSA-65 `01 ‖ 1952 bytes` (1953), ML-KEM-768 `01 ‖ 1184 bytes` (1185), standard base64. The Account ID
hashes the raw key after the id, so it does not change with typing. A key card is valid when:

1. `signing_public_key_b64` reads as a signing key of an algorithm this build knows and its raw key hashes to
   `account_id`;
2. `kem_binding_b64` is a **KEM binding** ([below](#kem-binding)) that the account made over that signing key and
   `kem_public_key_b64`.

A key of an algorithm this build does not know, or a binding of a newer version, is *update needed* (the account needs a
newer app here, `AppError::NewerFormat`), never "bad keys". The relay directory returns exactly these four fields with
every entry, so a directory answer and a card are checked the same way; the app's row readers (`DirectoryJson`,
the HTTP mesh-node list) read the fields by their shape as the mesh does (pp-cpp-mesh `MeshNodeHitCodec.h`): a malformed
field or a binding older than v2 is dropped, one of an unknown algorithm or a newer binding is kept and marks the row
`keys_update_needed`.

The app holds the keys of a card typed (the card store, `PeerSigningKeyStore`, `PeerKemKeyStore`); an identity holds its
own raw (`LocalIdentity`). The crossings are `lib/people/CardKeys.h`: `TypedKeyB64` (raw → typed), `RawSigningPublicKey`
/ `RawKemPublicKey` (typed → what `MlDsa`, `HybridKem::Encapsulate` and fingerprints take), `MakeKeyCard` (an identity's
key card, its binding signed).

### KEM binding

The account's statement that a KEM key is its own (`foundation/crypto/KemKeyBinding.h`), v2:

```
signed: "pp-browser:kem-binding" 00 ‖ u8 2 ‖ LenBytes signing key (typed) ‖ LenBytes KEM key (typed) ‖ ext area
blob:   u8 2 ‖ ext area ‖ LenBytes signature (typed)                                  → kem_binding_b64 (standard base64)
```

The binding rides other objects (key cards, account cards, directory rows, register/finish), so its version is the
blob's first byte. A receiver (`ReceiveKemBinding`, then `VerifyKemBinding` for the account) takes, in the receiver's
order: the version (none or 1: older, refused; above 2: update needed, nothing after it read), the area and the
signature's length with no byte after it (malformed), the three typed blobs (an unknown algorithm: update needed), the
signature over the rebuilt bytes (a forgery), the area's critical entries (update needed); then the signing key's hash
is the Account ID. The blob is kept as received, its area too. Vectors: `kem_binding_vectors.json`
(pp-specs `vectors/kem_binding_vectors.json`, `kem_key_binding_test.cpp`), which the relay reads byte for byte.

## Account card

A key card plus what the account says about itself, signed as a whole.

```json
{
  "card_version": 5,
  "account_id": "account:…",
  "signing_public_key_b64": "…",
  "kem_public_key_b64": "…",
  "kem_binding_b64": "…",
  "nickname": "alice",
  "devices": [
    { "peer_id": "Qm…", "signing_public_key_b64": "…", "kem_public_key_b64": "…" },
    { "peer_id": "Qm…", "signing_public_key_b64": "…", "kem_public_key_b64": "…" }
  ],
  "relays": [
    { "origin": "https://www.brief.global/api/relay", "relay_id": "relay:…" }
  ],
  "initiation_floor": 0,
  "issued_at": 1761235200000,
  "ext_b64": "…",
  "card_signature_b64": "…"
}
```

Every key and the signature are typed ([Key card](#key-card)); a device's keys may be of another algorithm than another
device's (an algorithm migration happens per device). `nickname` and `ext_b64` are left out when empty.

- `nickname` — the account's own name for itself: a hint, shown as such; never an identity
  ([N029](../../projects/p2p-mesh/NAME_DIRECTORY_NORTH_STAR.md): no second name system).
- `devices` — **the account's device list** (M021), the issuing device first, at least 1 and at most 5:
  each device's PeerId, the device ML-DSA-65 key that PeerId is derived from, and the device's own
  ML-KEM-768 key. This is the account's standing statement that those devices speak for it; nothing else
  in the system makes it.
- `relays` — **the relays the account receives on**, preferred first ([multi-relay MR002](../../projects/multi-relay/DECISIONS.md#mr002--the-relay-list-lives-in-the-account-card)):
  each named by its origin, the HTTPS base URL a client uses, and by its **relay id** (`relay_id`, `relay:` +
  base64url of 32 bytes, [SERVICE_ENDPOINTS § Relay identity](SERVICE_ENDPOINTS.md#relay-identity); release-compatibility
  SIGNED_FORMATS § 3.3): the relay's certificate under that id proves the PeerId a sender signs its send into that relay
  for ([SERVICE_ENDPOINTS § The audience](SERVICE_ENDPOINTS.md#the-audience-every-signed-command-names-its-relay),
  `RelayPins::ContactAudience`).
  Required; an empty list names no relay
  (the account's mailboxes are not known: a sender sends through its own preferred relay, [SERVICE_ENDPOINTS § Sending by the list](SERVICE_ENDPOINTS.md#sending-by-the-list)). Bounds ([MR004](../../projects/multi-relay/DECISIONS.md#mr004--a-relay-list-is-small-and-bounded)):
  at most 4 entries; each `origin` an absolute `https://` URL of at most 128 bytes with a host, and no query,
  fragment, credentials, spaces or control characters; each `relay_id` required, of the relay id's one written form
  (`IsRelayIdForm`: `relay:` and 43 base64url characters that decode to 32 bytes and encode back to themselves); no two
  the same relay by origin (scheme and host compared without case, `:443` as no port, an empty path as `/`) or by
  `relay_id`. The card carries no relay key: the sender checks the relay's certificate against the relay id, and the
  relay refuses a wrong audience (`wrong_audience`). The list holds the relays the app registers on whose pin is in
  force (`OwnCards::SetRelays` from `RelayPins::PinnedRelays`: a relay whose address names no relay id, or that does not
  prove it, is left off and the card re-issued once it is in force): My relays in their order, the preferred first, each
  with the relay id its address names (multi-relay MR008). A card from anyone else never writes a pin
  ([SERVICE_ENDPOINTS § Pins](SERVICE_ENDPOINTS.md#pins)); a linked device takes our own newer card's entries as
  pins. My relays is the profile's list, seeded from the
  deployment's relay and `relay.extra_base_urls` — [CONFIGURATION](../ops/CONFIGURATION.md),
  [SERVICE_ENDPOINTS § Several relays](SERVICE_ENDPOINTS.md#several-relays). A change to the list re-issues the card at
  once and announces it to our devices and contacts ([§ My relays](SERVICE_ENDPOINTS.md#my-relays));
  an origin outside the bounds — an `http://` dev relay — is left off, so a card of only such relays says the mesh
  only. The list is the **main device's**: the others keep the newest card's. Senders deliver into the listed
  relays in order ([SERVICE_ENDPOINTS § Sending by the list](SERVICE_ENDPOINTS.md#sending-by-the-list)).
  **Public-relay policy:** the origins come from a contact, so a sender makes a client only for one with a public
  host (`IsPublicRelayOrigin`, the one check for every relay not ours by configuration — a card's, a QR locator's,
  one typed in My relays): no loopback, private, link-local, CGNAT or reserved IP literal, no `localhost`,
  `*.localhost`, `*.local`, `*.internal` or single-label name; an origin outside it is skipped like an unreachable
  relay. Its requests also refuse any address that resolves non-public (checked after DNS), use no proxy and follow no
  redirect — the sends, the identity a locator's relay is asked for (MR007), and the stored card a Refresh or a QR
  contact asks a contact's relay for (`LookupStoredAccount` with `public_only`). The card, its signature and the list's bounds are unchanged, and our own relays are exempt. A Debug build
  with `PP_BROWSER_CA_BUNDLE` set (the lab) lifts the policy.
- `initiation_floor` — the account's price for opening a chat or a call with it, in `pp_credit` minor units
  ([pricing P001](../../projects/pricing/DECISIONS.md#p001--initiation-floor--media-quote-gates-no-payment-rails-yet),
  [MR005](../../projects/multi-relay/DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant)):
  required, an integer from 0 to 2^53 − 1 (the relay reads it as a JSON number). Every relay reads a stranger's price
  from it, and so does the app before a first message or call, the relay's `payment_required` over a card it came
  after ([SERVICE_ENDPOINTS § The stranger's price](SERVICE_ENDPOINTS.md#the-strangers-price)); the identity's `initiation_floor` (dogfood: `AppConfig.initiation_floor`) is the account's, and a device
  that takes a sibling's newer card takes its floor, as its nickname.
- `issued_at` — ms since epoch; the card's version. A device issues a new card only when what it says
  changes (a device joins, a device's keys, the nickname, the relays, the floor), always later than the newest it
  knows.
- No addresses: multiaddrs go stale and disclose IPs ([privacy](../../projects/privacy/DECISIONS.md));
  devices are reached through the mesh (below).

**Card signature:** the account key's typed signature (`card_signature_b64`) over (`AccountCardSignBytes`, the
[pp binary wire profile](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088): LenUtf8 / LenBytes with a u64 BE length, i64 BE):

```
"pp-browser:account-card" 00 ‖ u8 5
‖ LenBytes account signing key (typed) ‖ LenBytes account KEM key (typed)
‖ LenUtf8 nickname
‖ u64 devices, by peer_id bytes: LenUtf8 peer_id ‖ LenBytes signing key (typed) ‖ LenBytes KEM key (typed)
‖ u64 relays, in list order:     LenUtf8 origin ‖ relay_id (32 raw)
‖ i64 initiation_floor ‖ i64 issued_at
‖ ext area                                                       (SIGNED_EXTENSIONS § The extension area)
```

The preferred relay is signed as the first. The KEM binding is not in these bytes: it is its own signature. Golden
vectors: the SHA-256 of a fixed card's bytes (9797 of them) in `AccountCardSignBytesTest.MatchTheGoldenVector`
(`account_card_test.cpp`), and pp-specs `vectors/account_card_vectors.json` (
`account_card_vectors_test.cpp`): a signed card of keys from fixed seeds with its signed bytes, the same with one
unknown non-critical entry, its key card, and the cards that must be refused, each with its verdict. The relay
(`account-card.ts`) reads it byte for byte; `relay_sign_vectors.json` carries a card v5 too.

**Receiving a card** (`AccountCardFromJson` then `VerifyAccountCard`; `ReceiveAccountCard` runs both), in the
receiver's order ([SIGNED_EXTENSIONS § The receiver's order](SIGNED_EXTENSIONS.md#the-receivers-order)):

1. `card_version`: missing or below 5 — made by an older app, refused; above 5 — **update needed**, nothing else read
   (`IsNewerAccountCard`: the peer *needs a newer app*, U2; the card-ask gate stops asking for a version it cannot read);
2. the v5 fields and `ext_b64` (malformed: refused);
3. every typed blob — the account's keys, each device's, the signature (an unknown algorithm: update needed);
4. the signature over the rebuilt bytes, its area included (a forgery: refused);
5. an unknown critical entry in the area: update needed, the card is not acted on; unknown non-critical entries are
   ignored and kept (a stored or forwarded card keeps them, so its signature still verifies);
6. the key card (its binding's own order, then its signing key is the Account ID's), `devices` 1–5 entries with no
   duplicate PeerId and each `peer_id` the one its raw device signing key derives, `relays` and `initiation_floor`
   within their bounds, and `issued_at` not more than 5 minutes ahead of the receiver's clock.

A card breaking any of them is refused whole, by the app and the relay alike (`invalid_card_relays` at the relay for
the relay bounds). Cards of any other `card_version` are refused everywhere — registration (`invalid_account_card`),
directory answers, the card exchange, envelopes, group bundles, contact cards. An update-needed card is not a forgery:
the app marks the account as using a newer app; the relay answers `relay_upgrade_required`.

**Newest wins:** the newest valid card (`issued_at`) is the account's word on its keys, nickname,
device list, relays and floor; an older card changes nothing. A restored install keeps no cards but knows each account's newest
issue time from the account locker (`card_floors`, [ACCOUNT_LOCKER](ACCOUNT_LOCKER.md#state-json))
and refuses older cards. A device takes the newest card's nickname as the account's.

**Main device (master, [M021](../../projects/multi-device-account/DECISIONS.md#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) d4):** the first device on the list. Only it changes the list: takes a device off
(revoke: a newer list without it) or hands the role on (another device first). Any listed device re-issues the
card to bring its own entry or the nickname up to date, the list otherwise as it is — also a card of ours that no
longer verifies (stepped by a `profile.db` step), after our relays were asked for a newer one and none was found
([M026](../../projects/multi-device-account/DECISIONS.md#m026--the-main-device-keeps-the-list-keeping-the-card-signed-is-every-listed-devices)):
losing the main device freezes the list, nothing else. Only the main device links a device
([LINK_DEVICE.md](LINK_DEVICE.md), [M027](../../projects/multi-device-account/DECISIONS.md#m027--a-device-links-by-a-sealed-qr-handshake-the-seed-rides-sealed-each-device-its-own-dek)):
it seals the account's recovery seed to the new device's one-time key, with no card; the new device gets the card from
our relays by Account ID (as below, *A fresh list only on purpose*), takes its nickname and floor, and appends itself at
the end, once (`card_intent` join). For the link's 15 minutes after sealing, the main device keeps a device that only an
older card of the account lists — a join that reached it after it issued another card. When the main device takes in the card a
new device appended itself to, it sends that card to its contacts (the `device_list` notice), so their next
message reaches the new device too. A device restored from the recovery
phrase starts a fresh list holding only itself (`card_intent` fresh), newer than any it or the account's card
floor knew; so does a new account's first device (marked `fresh` when its identity is created). A device the newest
list leaves out is **signed out**: it issues no card, sends nothing of the account's (refused with the same signed-out error: chat sends and the call, group
invite-link and group membership / roster controls; still sent today: asks for a newer card, copy asks, device-list
notices and relay-grant messages, which use the control lane directly), and stops
syncing the account locker; Me → Security says so. The rule is followed by the account's own devices — each
still holds the account key, so a stolen device could sign itself back in (M021 limit 4).

**A fresh list only on purpose.** Our own card is kept twice ([DATA_LAYOUT](DATA_LAYOUT.md)): in `peer_cards` with everyone else's, and beside the
identity in `own_card.enc` (under the DEK, [AT_REST_ENCRYPTION § Own card plaintext](AT_REST_ENCRYPTION.md#own-card-plaintext-inside-own_cardenc)).
Every read of our card brings the older copy up to the newer (`OwnCards`), so Reset chats, the start-up recovery's
reset and a `profile.db` too old to upgrade — which remove the chat store, `peer_cards` with it — leave the account's
list as it was: our card goes back into `peer_cards` at the next start, nothing is issued, no sibling is signed out.
A device that holds **no card of its own and no `card_intent`** issues nothing: it asks our relays' directories for
the card we registered (`GET /v1/users/by-account/<our Account ID>`: our relays, preferred first, then the org's
directory; a relay that does not answer to its address is not asked, MR008), takes the newest that carries our
Account ID and account keys and verifies (kept as source `recovered`, which the own-account guard accepts), and goes
on as usual — listed, it refreshes its entry; left out, it is signed out. While none answered (offline) it waits
("Getting your device list…"; asked again after 5 s doubling to 5 min, at once when the network comes back or the
app comes to the front). When every relay asked answered and none keeps a card of ours (a relay that did not answer may keep it: then it is asked again later), Me → My devices asks the person:
*Start a new device list…* — its confirm says that the account's other devices are signed out — sets `fresh`.
(A registration while the card is still being got back carries none.)

**Size:** a key card is about 13 KB as JSON (a 3310-byte signature and two public keys, typed, in base64);
each device adds about 4 KB to an account card. Too large for a QR code, which is why invites carry a
locator.

## Locator (QR code, invite link)

What fits in a QR code: who to look for and where.

```
pp-browser-contact-v1:<base64url-unpadded(JSON)>
{ "account_id": "account:…", "nickname": "alice", "devices": ["Qm…", "Qm…"],
  "relay": { "origin": "https://www.brief.global/api/relay", "relay_id": "relay:…" } }
```

Unsigned: the Account ID is what the person showed or sent. The receiver fetches the account's card
from a listed device ([card exchange](#card-exchange-over-amp)) or, when one is configured, from the
relay directory, and accepts it only for that `account_id`. Listed devices are hints; the card decides.

- **`relay`** (optional; multi-relay R4, [MR004](../../projects/multi-relay/DECISIONS.md#mr004--a-relay-list-is-small-and-bounded),
  [MR008](../../projects/multi-relay/DECISIONS.md#mr008--a-relay-address-carries-its-peerid-no-first-use-trust)):
  the account's preferred relay's address — the relay the app registers on: its `origin` and its `relay_id` (the
  relay id its address names, SIGNED_FORMATS § 3.3), about 100 bytes — and nothing else of the card's list. The origin
  within the card's bounds (an absolute `https://` URL of at most 128 bytes with no query or fragment) and the relay id
  of its written form (`IsRelayIdForm`); one outside them, or without its relay id (an origin alone, or a `peer_id`
  entry of before the cutover), is left out when writing and dropped when reading (the locator still holds), so a lab's
  `http://` relay is never on it, as it is never on the card. It is optional: an app that does not know it ignores it,
  and a locator without it reads as before.
- A contact added from a locator keeps that address (`contacts.json` `remote.locator_relay`, `{origin, relay_id}`) as
  its relay list until its account's card is kept: a first message goes into that relay, signed for the PeerId that
  relay's certificate proves for the locator's relay id, as for a card entry's
  ([SERVICE_ENDPOINTS § Sending by the list](SERVICE_ENDPOINTS.md#sending-by-the-list)), and
  the contact page names it. From then on the card's list is the contact's
  ([`ContactRelays`](https://github.com/people-post/pp-cpp-core/blob/main/capabilities/people/src/lib/people/ContactIdentity.h)). A release build leaves out a
  `relay` outside the public-relay policy above (`IsPublicRelayOrigin`: on this device or a private network): it is
  neither kept nor asked for the card (developer builds take it, for the lab). The person is still added. The relay id
  is only the inviter's word, as a card's is: it names whom the first send is for, never a pin of ours.

- **Sharing:** the invite text (*Me → Profile → Invite*, and the share at the end of onboarding) ends
  with the locator line, so pasting the whole message works.
- **Adding:** *Add contact* finds a locator anywhere in pasted text (`FindContactLocator`). The person is
  added at once under the locator's nickname; the card is fetched from the listed devices (each reached
  through the mesh if no link is up), then from the locator's `relay` (the card the account registered there,
  `GET /users/by-account`), then the directory. With neither answering the contact stays and
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
| Relay directory | key card fields (+ endpoints), and the account card it registered with (`GET /users/by-account`) | when a directory is configured; a lookup's card is kept once it verifies ([SERVICE_ENDPOINTS § The directory on each](SERVICE_ENDPOINTS.md#the-directory-on-each)) |
| Relay registration | our account card (`register/finish` `account_card`) | every registration and renewal, and again when our card is newer than the one the relay last answered for; the relay reads our floor from it |
| Envelope | sender's account card, `body.e2e.sender_card` | when a copy carries a `key_init` ([MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md#master-psk)), so a new device's first message to anyone brings its account's newer list. On the control lane (a copy per device of the peer's, or the account copy — M024), only when a device it addresses may not hold it (`ControlLaneCarriesCard`, told per device of the peer's: a device copy's device, every device on the peer's card for an account copy): the first lane record this device sends each such device after the card changed (or since it started — a device just linked to the peer's account has been told nothing, whatever its siblings were), once a record, and always a `device_list` notice (a restored device sends it to every contact), a card or copy ask, a group's member check (the asker may be a restored device), a ringing call invite. The card is ~13 KB + 4 KB per device: it used to ride every lane message, and the relay filled with them. A receiver that cannot check an envelope — no card kept, or the sending device not on it — asks that device for the card (`card_ask`, at most once per account per 10 min and 8 a minute in all: the envelope is unverified, and its `sender_card_at` counts for nothing — [WIRE_SCHEMAS § card_ask](WIRE_SCHEMAS.md)); a lane record waits for it in memory (≤ 2 min) and is checked again once a card of the sender's is kept ([P2P_MESSAGING § The control lane](../architecture/P2P_MESSAGING.md#the-control-lane)). A device that finds a verified lane record with no copy for it, or an account copy it cannot open, sends the sender its own card (`device_list`, at most once per account per 10 min) |
| Envelope hint | how new the sender's list is, `body.e2e.sender_card_at` | on every envelope (M021 d6): a receiver holding an older card asks the sending device for the newer one (`card_ask` → a `device_list` notice; unsigned, so it never stops a later ask — `CardAskGate`), so a device taken out stops getting copies from peers the main device's announcement did not reach (a group member who is not a contact) |
| Our relays' directories | our own account card, by Account ID | linking a device: the new one fetches the list it signs itself into (M027; the link itself carries no card) |
| Sibling notice | the account card, on the control lane to our own mailbox (M022) | when a device's list changes (it linked in, a sibling's newer list, the name), and once a start while it has siblings: they take it |
| Contact card message | someone's account card (or key card, when that is all the sharer has), plus the sharer's name for them | "Share" on a contact; *Add to contacts* on the card |
| Locator | account id, nickname, devices, the preferred relay's address `{origin, relay_id}` (no card) | QR code, invite link, onboarding share |
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

**Refresh on the contact page.** The user's *Refresh account info* asks, in order of trust (multi-relay R3):

1. the contact's devices (the kept card's, then the contact's device rows) for the account's card even when one is
   kept (`ContactCardExchange::AskDevices`; at most once per account per 15 s, a press within that gets the last
   ask's outcome);
2. the contact's relays in list order (its newest kept card's, else its locator's origin), by Account ID
   (`GET /users/by-account/{account_id}`, public: asked without signing, whether or not we are registered there);
3. our relays, preferred first, then our other directories, skipping any asked in step 2.

The relays are asked up to the first whose answer for the account verifies; one down, one that does not know it, or
one whose answer does not verify gives way to the next. A contact with no Account ID asks no one (relays know people
by it alone, multi-relay MR001). An answer verifies when it is for the contact's Account ID and its row's key card
verifies against it (and its stored card, when it carries one that is not older than one this account had), or it
carries no keys but a stored card that verifies. **An unverified relay answer contributes nothing** — no keys, card,
name, photo or addresses: the relay is shown as not reachable on the contact page, and the user is told. A stored card
that verifies is kept as any other (newest wins), so its device and relay lists come in too. Name: the newest kept
card's when a device answered or a relay's card was kept, else the verified relay row's; photo and device addresses:
the verified relay row's. The toast says, in the plain *card* wording, what changed ("Got their latest card"), that it
is already up to date, or that nothing could be reached or found; it names no source. What each relay answered is kept in memory for the contact page, which shows
the preferred relay by host with its reachability at the last refresh (*Not known yet* before one asked it; *Mesh
only* when the card lists no relay) ([`ContactRefreshWorkflow`](../../capabilities/messaging/src/feature/conversations/ContactRefreshWorkflow.h)).

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

**The grant** (`RelayGrant`, grant v2: [release-compatibility SIGNED_FORMATS § 2.4](../../projects/release-compatibility/SIGNED_FORMATS.md#24-relay-grant-v2);
JSON on the send's `grant`, in the lane notice and in the account locker's `grants`):

```json
{ "grant_version": 2, "recipient_account_id": "account:bob…", "sender_account_id": "account:alice…",
  "issued_at": 1761235200000, "expires_at": 1763827200000, "ext_b64": "<optional>", "signature": "<base64>" }
```

`signature` is a **typed** signature ([SIGNED_EXTENSIONS § Typed blobs](SIGNED_EXTENSIONS.md#typed-blobs): algorithm
id `1`, then ML-DSA-65's 3309 bytes) by the **recipient's** account key over

```
"pp-browser:relay-grant" 00 ‖ u8 2 ‖ LenUtf8 recipient_account_id ‖ LenUtf8 sender_account_id
‖ i64 issued_at ‖ i64 expires_at ‖ ext area             (u64 BE lengths, big-endian integers)
```

`ext_b64` is the signed extension entries (absent when empty; none is defined at release 1). `expires_at − issued_at`
is at most **30 days**. A receiver runs [the receiver's order](SIGNED_EXTENSIONS.md#the-receivers-order)
(`ReceiveRelayGrant`): `grant_version` first, the fields and the area, the signature's algorithm id, the signature by
the recipient's key (its raw key hashes to the recipient's Account ID), an unknown critical entry; then the terms: the
pair, the lifetime, not expired. The relay checks it the same way with the recipient's registered key; the app with the
grantor's key resolved as [above](#resolving-keys). **A grant the app cannot read** (a newer `grant_version`, an
unknown algorithm, an unknown critical entry) is none, never a forgery: its grantor runs a newer app, and is not asked
for a grant again (`RelayGrantCoordinator::NeedsNewerApp`). A grant kept or forwarded keeps its entries. Vectors:
pp-specs `vectors/relay_grant_vectors.json` (the signed bytes of fixed grants, signed fixtures, each refusal's
verdict), read by the relay byte for byte.

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
verified envelope), at most once an hour; an ask is answered at once (at most every 10 min). A grant goes
again, confirmed or not, when its grantee's kept card comes to list a device the one before did not (a device it
linked or restored, which no earlier copy reached), at most once a minute. The grants given to an account also ride
its account locker ([ACCOUNT_LOCKER § Grants](ACCOUNT_LOCKER.md#grants)), so a restored or linked device holds them
before its first send — that send carries its new card, and a stranger's could be held back by the quota. An ask is
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
  (`PeerCards.h`): none while only a key card is known. A send to the account goes into those relays in order
  (`RecipientRelays`, [SERVICE_ENDPOINTS § Sending by the list](SERVICE_ENDPOINTS.md#sending-by-the-list)); when
  every one says the account is not registered there, its newest card is asked for — its devices over the mesh,
  then the directories of our relays and our other directory providers (`GET /v1/users/by-account`'s
  `account_card`), kept only once it verifies.
- Our own kept card is re-issued, its device list as it is, when it does not verify (`OwnCards`).

## Resolving keys

For an Account ID, signing and KEM keys come from, in order:

1. the card store;
2. a card fetched from one of the account's known devices over Amp (`get_card`);
3. the relay directory, when one is configured: the configured one, then our other relays', in order, the first that
   knows the account ([SERVICE_ENDPOINTS § The directory on each](SERVICE_ENDPOINTS.md#the-directory-on-each));
4. `[later]` the chain name registry (N029 phase D, [E024](../../projects/e2e-message-crypto/DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007)).

Every source goes through the same checks; none is trusted for more than carrying the bytes.

## Trust

A card proves *these keys and devices belong to this Account ID*. That the Account ID is the person
you think it is rests on how you got it — in person (QR), from them, from a friend's contact card, or
from the directory's search — exactly as with the directory today. The contact screen shows the
account fingerprint for comparing out of band (E016).
