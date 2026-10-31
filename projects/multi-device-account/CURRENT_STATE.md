# Multi-device account — current state

**As of:** 2026-10-30 (d1–d2 per-device sessions + M022 relay deployed; restore unsupported in the product, A011; account-state design M023 decided, not built)

Read [DECISIONS.md](DECISIONS.md) **M021**, **M022**, **M023** first — they supersede several older entries (each older entry says so in its *Superseded* / *Amended* line). Ordering and checkboxes: [PHASES.md](PHASES.md).

## Code today

| Area | Behavior |
|------|----------|
| Identity | Account ML-DSA-65 + account ML-KEM-768 + Account ID; each device its own ML-DSA (Peer ID) and ML-KEM key (M021 §1). A new account's keys derive from a recovery seed kept in `identity.enc` (M023 s1); accounts minted before have random keys and no phrase. No UI shows the phrase yet |
| Device list | Account card v2 (device entries signed by the account key, newest `issued_at` wins, ≤ 5 devices). Main device (d4, as a policy): the first on the list changes it — link (its transfer code), revoke, hand-over; a device left off is signed out; a restore starts a fresh list. Every device still holds the account key |
| Sessions | Per device pair for `e2e_public` and group copies (M021 d2); one relay record per account with a copy per device, own-account records allowed (M022, relay deployed) |
| Link | Paste bundle with the account keys, DEK and the issuing device's card — no sessions, no contacts (m4c superseded by M023) |
| Siblings | A new device's card reaches siblings by a control-lane `device_list` notice to our own mailbox (once per card version per start); the main device tells contacts of a newly linked one. What a device sends in a public chat or a group shows on its siblings (`sent_copy`, d3); reactions and deletes too; a device missing a copy asks the sender's device for one (M021 §9) |
| Account locker (M023) | Phrase shown in Me → Security behind the PIN; the locker synced with the relay 5 s after contact / group / card changes or a sibling's nudge (sent after its write), at each start and every 10 min — contacts both ways between the account's devices (adds, remarks, trust, removals; `ContactClock`); restore from a typed phrase works end to end (lab `restore_from_phrase`), offered on the fresh-install identity fork ("Restore with my recovery phrase"). Relay API on web2 www `feat/account-locker` (PR #215, not deployed). Format: [ACCOUNT_LOCKER.md](../../docs/contracts/ACCOUNT_LOCKER.md). The local account backup is removed from the code |
| Restore | **Unsupported in the product** (at-rest A011): no backup store, nothing written to the Keychain (old items deleted until 1.1.0). The old `LocalAccountBackup` path (contacts, groups, card floors) stays lab-only via the probe's file store until M023 s5 replaces it. A restored install is a new device: it raises card floors before receiving and announces its card to every contact |
| Directory | Account-first lookup, `endpoints[]` per device (M017) |

## Next

1. **Account state (M023)** — built: phrase, relay locker, contact sync between siblings (with a nudge), restore from a typed phrase. Saved copies (exported locker files): Me → Security "Save a copy of my account…", "Use a saved copy…" when restoring (iOS / Android pickers not yet tried on devices). Left: the recovery tiers (easy: synced keychain, needs a device; tier switch UI); the relay locker API deploys (www PR #215). pp-node is not a provider (M023 §6 amendment).
2. **d6** — the directory carries the card; card version in envelopes; re-decide E027 device-lock.
3. **d4 (cryptographic)** — only the master holds the account key: relay auth by device key + card, the seed off the link bundle (design first; the policy part is built).

## Stated limits in force

M021's limits (new devices have no history, revoke is eventual, …) and M023's (private tier: losing the phrase and every device loses the account; easy tier: the Apple / Google account is a way in; providers see locker size and timing).
