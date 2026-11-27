# Linking a device: the sealed QR handshake

**Tier:** contract (app ↔ app; no server reads it)

**Status:** Normative for release 1, from [multi-device M027](../../projects/multi-device-account/DECISIONS.md#m027--a-device-links-by-a-sealed-qr-handshake-the-seed-rides-sealed-each-device-its-own-dek)
(release-compatibility [INVENTORY § 4](../../projects/release-compatibility/INVENTORY.md#c-a-clean-start) C9). Replaces
the plaintext transfer code `pp-browser-link-device-v1`, which no build reads any more.
**Related:** [ACCOUNT_CARD § Main device](ACCOUNT_CARD.md), [AT_REST_ENCRYPTION](AT_REST_ENCRYPTION.md),
[ACCOUNT_LOCKER § Restore](ACCOUNT_LOCKER.md#restore), [SIGNED_EXTENSIONS](SIGNED_EXTENSIONS.md) (versions, typed
blobs, the extension area, the receiver's order), [SCANNING.md](../ui/SCANNING.md).

A new device joins the account of a **main device** (the first on the account's device list) in two payloads, each one
QR code or the same bytes as text:

1. The **new device** shows an **offer**: a fresh, one-time ML-KEM-768 public key.
2. The **main device** scans (or pastes) it, both show the same **4 digits**, and once the person confirmed they match
   the main device shows its **reply**: the account's recovery seed, sealed to that key.
3. The new device scans (or pastes) the reply, opens it, and brings the account here from the seed.

Neither payload holds a secret in the clear: the offer is a public key, and only the new device's one-time secret key,
which never leaves it, opens the reply. Both may be pasted anywhere (a chat, an email). The code is in
`capabilities/base/src/foundation/crypto/LinkHandshake.{h,cpp}`; the new device's key is held by
`lib/people/LinkOffer`, the account brought over by `lib/people/LinkDeviceExchange`.

## What a link carries, and what it does not

| Carried (sealed) | Why |
|------------------|-----|
| The account's 32-byte **recovery seed** | The account ML-DSA and ML-KEM keys, the locker key and the locker signing key derive from it ([M023](../../projects/multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) §2, `RecoverySeed.cpp`); the new device can show the same phrase |
| **Intent** `join` | The new device appends itself to the main device's list (`card_intent` join), unlike a restore's fresh list |
| An **expiry** (15 minutes from sealing) | A reply found later opens to nothing |
| The account's **preferred relay** `{origin, relay_id}`, only when it is not the deployment's | The new device asks it first for the account's card |

| Not carried | Where the new device gets it |
|-------------|------------------------------|
| The account key pairs | Derived from the seed, exactly as a restore from the phrase does |
| The account card (the device list) | Our relays' directories, by Account ID (`GET /v1/users/by-account`), the path a device without its card already has ([ACCOUNT_CARD § A fresh list only on purpose](ACCOUNT_CARD.md)) |
| The nickname and initiation floor | On that card: the new device takes them when it joins |
| The **DEK** | None: each device makes its own. Nothing sealed under a DEK crosses devices — every DEK-sealed file names its profile id in its AAD and stays on the install ([AT_REST_ENCRYPTION](AT_REST_ENCRYPTION.md)); what syncs (the locker, sessions, attachments) is sealed under other keys |
| Sessions, contacts, groups | Sessions are per device pair (M021); account state comes from the locker (M023 §9) |

An account minted before M023 s1 has no recovery seed and cannot link a device.

## The offer (new device → main device)

```
offer = u8 version (1)
      ‖ LenBytes(typed KEM public key)        u8 1 (ml-kem-768) ‖ 1184 bytes  → 1185
      ‖ ext area                              u64 BE length ‖ entries (empty at version 1)
```

1202 bytes at version 1. The key is the new device's alone and for this link only: a fresh pair each time the link page
opens (the one before is wiped), held in memory — never written, never sent — and wiped once the account came over, when
the person leaves the page, or 15 minutes after it was made.

## The reply (main device → new device)

```
reply  = u8 version (1)
       ‖ LenBytes(typed KEM ciphertext)       u8 1 (ml-kem-768) ‖ 1088 bytes  → 1089
       ‖ LenBytes(typed AEAD sealed)          u8 1 (xchacha20-poly1305) ‖ nonce (24) ‖ ciphertext ‖ tag (16)

key    = HKDF-SHA256(salt "pp-browser-link-v1", ikm = the KEM shared secret, info "link-reply-key"), 32 bytes
aad    = "pp-browser-link-reply-v1" ‖ 0x00 ‖ u8 version ‖ LenBytes(offer) ‖ LenBytes(typed KEM ciphertext)

bundle = Raw(32) recovery seed
       ‖ u8 intent                            1 = join
       ‖ i64 BE expires_at_ms
       ‖ u8 relays                            0 or 1
       [ ‖ LenUtf8 origin ‖ Raw(32) relay id ]  an origin of at most 128 bytes (the card's bound), the relay id's bytes
       ‖ ext area
```

The bundle is 50 bytes with no relay (the reply 1197 bytes), 218 with a relay of the longest origin (the reply 1365). The AAD names
the offer, so a reply opens only for the offer it answers: not for another device's, nor for an older offer of the same
device. The tags are hash and KDF labels, not signing domains (SIGNED_EXTENSIONS § Signed bytes), and keep their `-v1`.

## The 4 digits

```
code = (first 4 bytes of BLAKE2b-256("pp-browser-link-code-v1" ‖ 0x00 ‖ offer), as u32 BE) mod 10000, 4 digits
```

The new device shows them under its QR code; the main device, once it read the offer, asks *Does the new device show
1234?* and seals **only** after the person said yes. The check comes before sealing because that is the step that sends
the seed: an offer swapped in — pasted from a chat, a QR code on someone else's screen — shows other digits, and the
person stops before anything leaves. A code over the offer and the reply's ciphertext would only be shown after the
reply exists, when the seed is already out. Limits: 4 digits are 1 in 10,000 against a guess, and someone who sees the
real new device's digits as they appear could search for a key of their own with the same digits (about 5,000 key pairs);
the check protects against a mistaken or planted code, not against an attacker who watches the new device's screen and
puts their own code before the main device's camera at the same time. The new device does not check the main device:
it joins whichever account's reply it is shown (the person holds both devices).

## Text forms and QR codes

| Payload | Text | QR code |
|---------|------|---------|
| Offer | `pp-browser-link-offer:` ‖ base64url-unpadded(offer), 1625 characters | binary mode, error correction M: **version 29** (133 modules) |
| Reply | `pp-browser-link-reply:` ‖ base64url-unpadded(reply), about 1620 characters | binary mode, error correction M: **version 29**, **30** with a relay of the longest origin |

The QR code holds the bytes themselves (byte mode, `lib/ui/QrSvg` `QrSvgContentBinary`); the in-app scanner returns them as
read (`lib/scan` / quirc). A reader takes the text form anywhere in what was pasted (after its prefix, up to the first
character that is not base64url), or a scanned payload as it is when its first byte is below `0x20` (so a QR code of
text is never read as a payload). The text is never rendered into a field (B166/B167: over a kilobyte): the pages copy
it to and read it from the clipboard.

## The receiver's order

For both payloads, in the order of [SIGNED_EXTENSIONS § The receiver's order](SIGNED_EXTENSIONS.md#the-receivers-order),
the AEAD standing in for the signature:

| Step | Failure |
|------|---------|
| 1. Version: the first byte | 0 or missing: unreadable. Above 1: **update needed** (`AppError::NewerFormat`), nothing else read |
| 2. Fields | unreadable (lengths, bytes left over, an area that does not parse) |
| 3. Algorithm ids (the typed key, ciphertext and AEAD blob) | unknown: update needed; else unreadable |
| 4. Open (reply only): decapsulate with the one-time key, AEAD with the AAD | **not for this device** (another device's reply, altered, or for another offer); no key (gone, out of time): expired |
| 5. Extensions, then the bundle's fields | an unknown critical entry or an unknown intent: update needed; fields: unreadable |
| 6. Semantics | `now ≥ expires_at_ms`: **expired** |

The errors are `LinkError` (`Unreadable`, `NotForThisDevice`, `Expired`) in `Error::category` `kLinkErrorCategory`, apart
from `AppError::NewerFormat`; the pages word each (`gui/LinkDeviceText`).

## On each device

- **Main device** (Me → Security → Link a new device): only the main device, not signed out, registered, with a card of
  its own (M021 d4). With an own PIN the PIN comes first. It reads the offer (camera or clipboard), shows the digits, and
  after the person's yes seals the reply, shows it as a QR code with *Copy as text*, and expects a join for the link's
  15 minutes (a device that only an older card lists is kept, [ACCOUNT_CARD § Main device](ACCOUNT_CARD.md)).
- **New device** (first start → *I already have an account on another device* → a PIN for this device): the link page
  shows the offer's QR code, its digits and *Copy as text*, and takes the reply by *Scan* (the gate steps aside for the
  camera) or *Paste*, then *Continue*. Opening it, the device derives the account from the seed (`LinkDeviceExchange::ImportAccount`:
  its own device keys and PeerId kept, a DEK of its own wrapped with its PIN, `card_intent` join, registered), makes the
  bundle's relay its preferred one, asks our relays for the account's card and keeps it (its nickname and floor become
  this device's); its first card appends it to that list, and its siblings and the main device learn it as today. A relay
  that does not answer then leaves the card to the usual asking once messaging is up.

A restore from the recovery phrase brings the account the same way (`ImportAccount` with intent fresh and the locker's
nickname); only how the seed arrives differs.

## Vectors

pp-specs `vectors/link_handshake_vectors.json`: an offer from fixed coins, its text and
its code (frozen), and two replies that key opens (randomized: compared without `reply_b64`, each checked to open to its
bundle). `PP_WRITE_LINK_HANDSHAKE_VECTORS=<path>` writes a fresh file.

## Versions

| Object | Release 1 | Floor | A later version |
|--------|-----------|-------|-----------------|
| Offer | 1 | 1 | read as "update needed" by an older main device |
| Reply | 1 | 1 | read as "update needed" by an older new device |
