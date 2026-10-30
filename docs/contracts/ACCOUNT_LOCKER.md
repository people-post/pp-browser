# Account locker: account state any provider can hold

**Tier:** contract

An account's state beyond its keys — the relay binding, nickname, contacts, groups, card floors — lives in
an **account locker**: one encrypted snapshot, sealed and signed with keys derived from the account's
**recovery seed** ([M023](../../projects/multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold)).
The provider holding it (the relay; later other relays) cannot read it or forge it, and keeps only the newest version.
The same snapshot is how linked devices share contact edits and how an account comes back on a fresh
install from its recovery phrase.

Code: `foundation/crypto/RecoverySeed` (keys), `lib/people/AccountLocker` (seal / open / merge),
`lib/people/ContactClock` (contact registers), `feature/conversations/AccountLockerWorkflow` (sync),
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

## Snapshot

| Field | Meaning |
|-------|---------|
| `locker_id` | as above |
| `version` | u64; each write picks `max(now ms, highest version seen + 1)` |
| `signing_public_key` | the locker ML-DSA-65 public key (its hash must be `locker_id`) |
| `sealed` | ≤ 1 MiB + 256 bytes: XChaCha20-Poly1305 under the locker key, AAD `account-locker\|{locker id hex}\|{version}`, of the state JSON prefixed by its length (u32 BE) and zero-padded to 4 KiB × 4ⁿ |
| `signature` | locker key over `pp-browser:account-locker-v1` ‖ 0 ‖ u8 1 ‖ LenBytes(locker id) ‖ u64 `version` ‖ LenBytes(`sealed`) |

A reader accepts a snapshot only when the signature verifies, the id matches the key, and the version is
not below the **highest version this device has seen for that locker** (rollback protection: a provider
serving an old snapshot is refused). Among several, the newest valid one wins (`PickNewest`). Golden
vectors: `account_locker_test`, `http_locker_client_test` and www `__tests__/AccountLocker.test.ts`.

## State JSON

| Key | Content |
|-----|---------|
| `relay_user_id` | the account's relay binding (restore needs it: no locker, no restore) |
| `nickname` | the account's nickname |
| `contacts` | address book rows (`ContactToJson`) |
| `groups` | per group: metadata, roster as identities with roles, the owner-signed roster statement — no messages ([group-chat G013](../../projects/group-chat/DECISIONS.md)). A group the account is out of (left, or removed — the member check found it out) is a tombstone: `group_id`, `owner`, `roster_epoch`, `left: true` |
| `card_floors` | `{account_id, issued_at}`: the newest account card issue time seen per account — a restored device refuses older cards ([ACCOUNT_CARD § Newest wins](ACCOUNT_CARD.md)) |
| `providers` | provider list (reserved; empty until a second relay exists). pp-nodes are never providers: they keep caches only (M023 §6 amendment) |
| `contact_clock` | one register per contact account: `{account_id, at, present, digest}` (below) |

Every key is optional on read; unknown keys are ignored.

## Sync between an account's devices

One sync (`AccountLockerWorkflow::Publish`), serialized per device:

1. Read the locker (as above; none yet is fine).
2. **Note local edits:** each contact account gets a register — `present`, `digest` (first 32 hex of
   BLAKE2b-256 over the user's fields: display name and trust) and `at`. A change sets
   `at = max(now ms, previous at + 1)`; a removed contact keeps its register with `present = false`
   (a tombstone, so the removal carries).
3. **Take remote edits:** a register newer than ours wins — present: the remote user's fields go into
   this device's row (keeping its own id and directory data); absent: the contact is removed here.
4. Store the merged state: contacts by register; groups by the higher `roster_epoch`; card floors by
   the higher `issued_at`; providers by union. Groups: the higher `roster_epoch` wins; at the same epoch a tombstone wins, so an older copy where the account was a member does not bring a left group back, while a later re-invite (a higher epoch) does. **Nothing new → no write.**
5. After a write, nudge the siblings: a `device_list` control-lane notice to the account's own mailbox,
   only when the own account card lists more than one device. A device that receives a verified sibling
   notice syncs 5 s later; since a sync with nothing new writes nothing, the exchange stops.

Triggers: 5 s after a contact, group or card change, or a sibling's nudge; at each start; every 10 min.
Only an account with a recovery seed and a relay id syncs.

Limits: registers use device wall clocks, so only edits made **concurrently** on two devices are
ordered by clock (an edit made after seeing another always wins). Tombstones (contacts and groups) are kept;
nothing prunes them yet.

## Restore

On a fresh install, a typed phrase → seed → locker (fetched by id alone) → the link bundle the seed
reconstructs (`RecoveryBundle`: derived account keys, the seed, a fresh DEK, the locker's relay id and
nickname) imported under the PIN the user chooses → contacts written → once messaging is up: card floors
before anything is received, groups recreated (tombstones skipped) and checking their members, the device announced, the
locker synced.

## Saved copy (exported locker file)

The user can keep a copy of the locker wherever they like (M023 §6 amendment): `ExportFileJson` seals the state as
this device has it — no network — at a version above any seen (so the provider's next write goes higher still),
as JSON `{format: "pp-browser-account-locker-file-v1", exported_at, locker: {locker_id, version,
signing_public_key_b64, sealed_b64, signature_b64}}` (the record the relay stores). Restore takes the phrase and
any saved copies: the newest valid snapshot among them and the provider's wins (same checks as above); with a
saved copy an unreachable provider is not an error. A file of another phrase, or not a saved account, is refused.
UI: Me → Security → **Save a copy of my account…** (`pp-account-YYYY-MM-DD.json`, the platform save / export
picker); the restore page's **Use a saved copy…** (the open picker).

## On disk

`{data_dir}/profiles/{id}/account_locker.json` (plaintext, atomic write):
`highest_version_by_locker` (`{locker id hex: version}`) and `contact_clock` (the registers above). See
[DATA_LAYOUT](DATA_LAYOUT.md).
