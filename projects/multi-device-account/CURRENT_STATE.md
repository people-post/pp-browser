# Multi-device account — current state

**As of:** 2026-10-30 (d1–d2 per-device sessions + M022 relay deployed; restore unsupported in the product, A011; account-state design M023 decided, not built)

Read [DECISIONS.md](DECISIONS.md) **M021**, **M022**, **M023** first — they supersede several older entries (each older entry says so in its *Superseded* / *Amended* line). Ordering and checkboxes: [PHASES.md](PHASES.md).

## Code today

| Area | Behavior |
|------|----------|
| Identity | Account ML-DSA-65 + account ML-KEM-768 + Account ID; each device its own ML-DSA (Peer ID) and ML-KEM key (M021 §1). A new account's keys derive from a recovery seed kept in `identity.enc` (M023 s1); accounts minted before have random keys and no phrase. No UI shows the phrase yet |
| Device list | Account card v2 (device entries signed by the account key, newest `issued_at` wins, ≤ 5 devices); every linked device still holds the account key (master device is d4) |
| Sessions | Per device pair for `e2e_public` and group copies (M021 d2); one relay record per account with a copy per device, own-account records allowed (M022, relay deployed) |
| Link | Paste bundle with the account keys, DEK and the issuing device's card — no sessions, no contacts (m4c superseded by M023) |
| Siblings | A new device's card reaches siblings by a control-lane `device_list` notice to our own mailbox (once per card version per start). Sent messages do not show on siblings yet (d3) |
| Account locker (M023) | Phrase shown in Me → Security behind the PIN; the locker published to the relay 5 s after contact / group / card changes and at each start (merged with what it holds); restore from a typed phrase works end to end (lab `restore_from_phrase`) but has **no fresh-install UI yet**. Relay API on web2 www `feat/account-locker` (local, not deployed) |
| Restore | **Unsupported in the product** (at-rest A011): no backup store, nothing written to the Keychain (old items deleted until 1.1.0). The old `LocalAccountBackup` path (contacts, groups, card floors) stays lab-only via the probe's file store until M023 s5 replaces it. A restored install is a new device: it raises card floors before receiving and announces its card to every contact |
| Directory | Account-first lookup, `endpoints[]` per device (M017) |

## Next

1. **d3** — sent messages on our own siblings; ask for a missing copy; a linked device introduces itself to contacts.
2. **s1–s2** — recovery seed with its two tiers (easy: synced keychain, the default; private: the phrase) and the locker format (client only; can start now).
3. **s3** — relay locker API (relay change first), then pp-node provider.
4. **d4** master device, **s4** change log, **s5** restore / link from the locker, **s6** lab + promotion.

## Stated limits in force

M021's limits (new devices have no history, revoke is eventual, …) and M023's (private tier: losing the phrase and every device loses the account; easy tier: the Apple / Google account is a way in; providers see locker size and timing).
