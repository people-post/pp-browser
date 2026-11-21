# Account locker: account state any provider can hold

**Tier:** contract

An account's state beyond its keys — the relay binding, nickname, contacts, groups, card floors — lives in
an **account locker**: one encrypted snapshot, sealed and signed with keys derived from the account's
**recovery seed** ([M023](../../projects/multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold)).
The provider holding it (the relay; later other relays) cannot read it or forge it, and keeps only the newest version.
The same snapshot is how linked devices share contact edits and how an account comes back on a fresh
install from its recovery phrase.

Code: `foundation/crypto/RecoverySeed` (keys), `lib/people/AccountLocker` (seal / open / merge),
`lib/people/ContactClock` (contact registers), `lib/people/AccountStateCodec` (rows, unknown fields kept), `feature/conversations/AccountLockerWorkflow` (sync),
`ConversationsHub::RestoreFromRecoveryPhrase` (restore). HTTP API: [SERVICE_ENDPOINTS § HTTP account
lockers](SERVICE_ENDPOINTS.md#http-account-lockers-multi-device-m023). The seed on disk:
[AT_REST_ENCRYPTION § Identity plaintext](AT_REST_ENCRYPTION.md).

## Keys

The recovery seed is 32 random bytes; the recovery phrase is its BIP-0039 English encoding (24 words,
with checksum) — the seed itself, never BIP-0039's PBKDF2 output. HKDF-SHA256, salt
`pp-browser-recovery-v1`:

| Label | Bytes | Use |
|-------|-------|-----|
| `account-ml-dsa` | 32 | account ML-DSA-65 key pair (seed) — the Account ID follows from it |
| `account-ml-kem` | 64 | account ML-KEM-768 key pair (coins) |
| `locker-key` | 32 | XChaCha20-Poly1305 key of the snapshot |
| `locker-signing` | 32 | locker ML-DSA-65 key pair (seed) |

**Locker id** = BLAKE2b-256 of the locker ML-DSA public key (lowercase hex on the wire). Anyone with the
phrase finds the locker; nobody without it can link the locker to the account.

## Record (locker record v2)

What a provider stores and serves, verbatim ([release-compatibility SIGNED_FORMATS § 2.6](../../projects/release-compatibility/SIGNED_FORMATS.md#26-account-locker-record-v2-and-state);
the shared rules: [SIGNED_EXTENSIONS](SIGNED_EXTENSIONS.md)). The JSON (`POST /v1/lockers/put`'s and `GET
/v1/lockers/:id`'s `locker`, a saved copy's `locker`):

| Field | Meaning |
|-------|---------|
| `record_version` | **2**, read first: missing or lower is an older app's (refused), higher a newer app's ("update needed", nothing else read) |
| `locker_id` | as above (lowercase hex of 32 bytes) |
| `version` | the revision, u64; each write picks `max(now ms, highest version seen + 1)` |
| `signing_public_key_b64` | the locker ML-DSA-65 public key, **typed** (algorithm id `1`, then the 1952-byte key); the BLAKE2b-256 of the raw key (without the id) must be `locker_id` |
| `sealed_b64` | ≤ 1 MiB + 256 bytes: XChaCha20-Poly1305 under the locker key, AAD `account-locker\|{locker id hex}\|{version}`, of the state JSON prefixed by its length (u32 BE) and zero-padded to 4 KiB × 4ⁿ — `FileCipher`'s bytes, whose first byte (`1`) is the AEAD id |
| `ext_b64` | the signed extension entries (SIGNED_EXTENSIONS § The extension area); absent when empty (none is defined at release 1). Unknown non-critical entries are kept: a record is stored and served as received |
| `signature_b64` | the locker key's **typed** signature over the signed bytes below |

Signed bytes (`account_locker::SignBytes`):

```
"pp-browser:account-locker" 00 ‖ u8 2 ‖ LenBytes locker_id ‖ u64 version ‖ LenBytes sealed ‖ ext area
```

(u64 BE lengths; the ext area is the u64 BE length of the entries, then the entries.) The signing key is not in them: the
locker id it hashes to is.

**The receiver's order** (`account_locker::ReceiveRecord`; the relay's `lockers/put` runs the same): `record_version`;
the fields and the area (malformed: refused); the algorithm ids of the key, the signature and `sealed` (unknown: update
needed); the signature — the raw key hashes to `locker_id` and the signature verifies (else a forgery); an unknown
critical entry (update needed, only after the signature). A device then opens the seal, reads `state_version` first
(below), and accepts a snapshot only when its revision is not below the **highest version this device has seen for
that locker** (rollback protection: a provider serving an old snapshot is refused). Among several, the newest valid one
wins (`PickNewest`); **one a newer app wrote** (a higher `record_version` or `state_version`, an unknown algorithm or
critical entry) is skipped and noted: the device reads the newest it can and **stops publishing** with "update needed"
(`AppError::NewerFormat`), never writing over what it cannot read. Vectors: `lib/people/tests/data/account_locker_vectors.json`
(the signed bytes of fixed inputs, signed fixtures and each refusal's verdict), read by the relay byte for byte.

## State JSON

Inside the seal (not signed: the seal is the locker key's).

| Key | Content |
|-----|---------|
| `state_version` | **1**, read first once a snapshot opens (INVENTORY T7): missing or lower is refused; higher is a newer app's — the device says "update needed" and does not write the locker |
| `nickname` | the account's nickname |
| `contacts` | address book rows (`ContactToJson`) |
| `groups` | per group: metadata, roster as identities with roles, the owner-signed roster statement — no messages ([group-chat G013](../../projects/group-chat/DECISIONS.md)). A group the account is out of (left, or removed — the member check found it out) is a tombstone: `group_id`, `owner`, `roster_epoch`, `left: true`. A group the owner dissolved (group-chat G016) is a terminal tombstone: `left: true, dissolved: true` |
| `card_floors` | `{account_id, issued_at}`: the newest account card issue time seen per account — a restored device refuses older cards ([ACCOUNT_CARD § Newest wins](ACCOUNT_CARD.md)) |
| `grants` | the relay grants other accounts gave this one ([ACCOUNT_CARD § Relay grants](ACCOUNT_CARD.md#relay-grants), multi-relay MR005), each **grant v2** as received: `{grant_version: 2, recipient_account_id, sender_account_id, issued_at, expires_at, ext_b64?, signature}` (the grantor, this account, Unix ms, its extension entries, the grantor's typed signature). A restored or linked device takes them so its first sends to a grantor are not a stranger's (below). A row of another `grant_version` is dropped on read |
| `relays` | our relays (My relays, multi-relay MR006): `{list: [{origin, relay_id}, …], card_issued_at}`, preferred first, as the account card issued at `card_issued_at` lists them, each with its relay id (`relay:` + 43 base64url characters, [SIGNED_FORMATS § 3.3](../../projects/release-compatibility/SIGNED_FORMATS.md#33-what-carries-the-relay-id-instead-of-peer_id): the relay proves it with its certificate); at most 4, each origin once (more, and an entry without a relay id in its one spelling, are dropped on read). A restored device takes them before its first card, so a restore keeps the person's list instead of the deployment's default. Absent when none |
| `providers` | provider list (reserved; empty until a second relay exists). pp-nodes are never providers: they keep caches only (M023 §6 amendment) |
| `contact_clock` | one register per contact account: `{account_id, at, present, digest}` (below) |
| `device_names` | the display names of the account's devices: `[{peer_id, name, at}]`, or `{peer_id, at, removed: true}` for a removed device (below). Never in the account card, a log or a metric |

Every key but `state_version` is optional on read. **Writers keep what they do not know** (INVENTORY T7): the state
remembers the JSON it was read from (`AccountLockerState::kept`), and a write — a merge and publish, or a state written
back as read — puts back every top-level key this build does not read, and on each row the fields it does not read from
the stored row of the same key (contacts by `id`, also inside `local` and `remote`; groups by `group_id` and their
members by `identity`; card floors and clock registers by `account_id`; grants by grantor and `issued_at`; relays by
`origin`, and the `relays` object's own; device names by `peer_id`). A field this build writes is its own (a kept value
never overrides it); a row this device no longer writes takes its fields with it. So a newer device's additive fields
survive an older device's publish within one `state_version`.

### Grants

A grant is the grantor's signed statement, so it proves itself: a device keeps one from the locker only when it is to this
account, newer than the one it holds from that grantor, and checks against the grantor's account key (resolved by its
Account ID, as a grant received over the control lane is — `RelayGrantCoordinator::TakeFromLocker`); the seal says only
that one of the account's devices wrote it. Kept, it frees what the grantor's stranger quota held back, as a grant
received does. **Merge and cap** (`CapGrants`, the same on every device): none expired, one per grantor (the later
`issued_at`; the larger `signature` on a tie), at most 32 (`kMaxLockerGrants`; the latest issued stay: about 4.6 KB
each, so the snapshot stays in a small size bucket), sorted by grantor. A row with a field missing, longer than 8 KiB, or
expiring before it was issued is dropped on read. A grantor cut by the cap still grants a restored device once it hears of
it (its grantee's card lists a new device).

### Device names

`peer_id` is a device's PeerId, `name` its display name, `at` Unix ms. **Per `peer_id` the larger `at` wins, whoever wrote
it**; the stored copy stands on a tie. Names are untrusted input and normalised **when read** (`NormalizeDeviceName`):
invalid UTF-8, control characters and line breaks dropped, ends trimmed, at most 32 Unicode code points (cut by code
point; a combined emoji may be cut in the middle), bidi controls (U+202A–202E, U+2066–2069, U+200E, U+200F, U+061C) and
zero-width characters (U+200B, U+200C, U+FEFF) dropped (U+200D stays), an empty result means no entry. `at` is clamped
on read to `0 … 4102444800000` (the year 2100). One entry per `peer_id`.

**Removal tombstone.** Taking a device off the account writes `{peer_id, at, removed: true}` (no `name`) into the same
table, with `at = max(now, the old at + 1)`. It merges like any entry (the larger `at` wins), so a device that still holds
the removed device's old name cannot put it back; a tombstone is kept when read, shown nowhere and is not an "entry" for
the "no entry yet" test below (a device still on the card that finds itself tombstoned and publishes its default name
makes a newer live entry). A tombstoned `peer_id` is not renamed.

**Cap.** At most 8 entries, live and tombstones together (`kMaxDeviceNameEntries`). Over that, the cut does not look at
the card or at who is asking (every device cuts the same): tombstones go first (the oldest `at` first), then live entries
(the smallest `at` first); the larger `peer_id` goes first on a tie. An entry the cap cut is not merged back from the
local table. Rows are kept sorted by `peer_id`, so equal states serialize equal.

A name is only ever set to a non-empty value (an empty input is a cancel). A device may name itself; the **main device**
(the own account card's first) may also name another device on the card — a client rule: the locker cannot tell which
device wrote an entry, and a device taken off the account that still holds the seed can write too (it changes display
only; which device is main is the card's). A device without an entry is shown as "Unnamed".

## Sync between an account's devices

One sync (`AccountLockerWorkflow::Publish`), serialized per device:

1. Read the locker (as above; none yet is fine).
2. **Note local edits:** each contact account gets a register — `present`, `digest` (first 32 hex of
   BLAKE2b-256 over the user's fields: display name and trust) and `at`. A change sets
   `at = max(now ms, previous at + 1)`; a removed contact keeps its register with `present = false`
   (a tombstone, so the removal carries).
3. **Take remote edits:** a register newer than ours wins — present: the remote user's fields go into
   this device's row (keeping its own id and directory data); absent: the contact is removed here.
4. Store the merged state (device names: this device's table and the stored one by `peer_id`, the larger `at` wins; a stored
   state without the key erases nothing; **publishing never prunes entries by the publisher's own card** — cards reach devices
   at different times; a removal is a tombstone entry (above), and names are filtered by the card only when shown;
   **this device's system name is added only while no live entry for it exists anywhere**, never over a given name; `at`
   moves only when a name actually changes), the same whichever device merges (at a tie the stored register, row and group stand —
   rows hold each device's own local fields; lists sorted): contacts by register; groups by the higher `roster_epoch`; card floors by
   the higher `issued_at`; grants by `CapGrants` over both copies (the stored ones are first taken here, each checked);
   relays from the copy with the higher `card_issued_at` (ours on a tie); providers by union. Groups: the higher `roster_epoch` wins; at the same epoch a tombstone wins, so an older copy where the account was a member does not bring a left group back, while a later re-invite (a higher epoch) does. A `dissolved` tombstone is terminal: it wins over any copy of the group whatever its epoch (a dissolved group is never re-invited), a restore skips it, and a device that is still in the group ends it at its next sync (the locker is the account's own state). **Nothing new → no write.**
5. After a write, nudge the siblings: a `device_list` control-lane notice to the account's own mailbox,
   only when the own account card lists more than one device. A device that receives a verified sibling
   notice syncs 5 s later; since a sync with nothing new writes nothing, the exchange stops.

Triggers: 5 s after a contact, group or card change, a device being named, a device joining the main device's list, or a sibling's nudge; at each start (a newly linked device's first sync, so its system name reaches the others); every 10 min.
Only an account with a recovery seed and registered with the relay syncs (the relay keeps a locker for an account registered there, by its Account ID — multi-relay MR001).

Limits: registers use device wall clocks, so only edits made **concurrently** on two devices are
ordered by clock (an edit made after seeing another always wins). Tombstones (contacts and groups) are kept;
nothing prunes them yet.

## Restore

On a fresh install, a typed phrase → seed → locker (fetched by id alone) → the link bundle the seed
reconstructs (`RecoveryBundle`: derived account keys, the seed, a fresh DEK, the locker's
nickname; the account counts as registered) imported under the PIN the user chooses → contacts written → our relays
(`relays`) become My relays, before the restored device issues its first card → once messaging is up: card floors
before anything is received, groups recreated (tombstones skipped) and checking their members, the grants taken (each
checked, before the announce, so its sends carry them), the device announced, the locker synced.

## Saved copy (exported locker file)

The user can keep a copy of the locker wherever they like (M023 §6 amendment): `ExportFileJson` seals the state as
this device has it — no network — at a version above any seen (so the provider's next write goes higher still),
as JSON `{format: "pp-browser-account-locker-file-v1", exported_at, locker: {record_version, locker_id, version,
signing_public_key_b64, sealed_b64, ext_b64?, signature_b64}}` (the record the relay stores, [§ Record](#record-locker-record-v2);
the file's own shape and `format` are unchanged by record v2). Restore takes the phrase and
any saved copies: the newest valid snapshot among them and the provider's wins (same checks as above); with a
saved copy an unreachable provider is not an error. A file of another phrase, or not a saved account, is refused.
UI: Me → Security → **Save a copy of my account…** (`pp-account-YYYY-MM-DD.json`, the platform save / export
picker); the restore page's **Use a saved copy…** (the open picker).

## On disk

`{data_dir}/profiles/{id}/account_locker.json` (plaintext, atomic write): `schema_version` 1 (a file without it, from
before it, is 1; a newer one is not read and never written over — that run keeps its state in memory),
`highest_version_by_locker` (`{locker id hex: version}`), `contact_clock` (the registers above) and `device_names` (the
table as last merged, plus this device's own edits not yet published; plaintext like the contacts' nicknames). See
[DATA_LAYOUT](DATA_LAYOUT.md).
