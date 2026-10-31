# Group chat — current state

**Last updated:** 2026-10-30 (G015 mentions implemented, m1–m6; signed roster + member check G012 / G013 merged in #411)

Ordering and checkboxes: [PHASES.md](PHASES.md). Read [DECISIONS.md](DECISIONS.md) G012–G015 before touching rosters or mentions.

## Landed

| Area | State |
|------|-------|
| Base group chat (C0–C5) | Create / invite / accept / remove / leave / transfer / rename / fork; N ciphertexts per message; roster in `GroupRosterStore` |
| Per-pair numbering (G010 / G011) | Implemented |
| Owner-signed roster (G012) | `lib/messaging/GroupRosterStatement.*`; roster events carry the statement, `owner_transferred` the signed hand-over; re-signed after `member_left` (#411) |
| Member check (G013) | `lib/messaging/GroupRosterSync.*`; request / reply on the control lane (`GroupRosterSyncFamily`); sends refused while checking / removed (#411) |
| Groups in the account locker | Groups ride in the encrypted account locker (multi-device M023): a restore from the recovery phrase brings them back checking their members (G013); a group left is kept as a tombstone |
| Size limit (G014) | 50 active members; the owner refuses create / invite past it and refuses a late accept (`group_invite_refused`); the invitee's group goes read-only |
| Dissolve (G016) | The owner dissolves a group for everyone: `group_dissolved` (owner-signed roster statement with `dissolved`) to each member. Ending is one transaction with the sends still owed (`GroupRosterStore::EndGroup`, `group_pending_sends`), resumed at start and after failures (`GroupMembershipWorkflow::ResumePendingGroupSends`). Receivers judge it like any owner statement (`ApplyDissolvedStatement`: owner, epoch, `OwnerHandedOn`), empty the roster and mark the group `dissolved`; the chat stays read-only with one line; sends, invites, traffic, checks refused or dropped; a dissolved group answers checks with the dissolve; outstanding invitees get `group_invite_refused` (`group_dissolved`); the owner's other devices hear on the control lane and from the locker's terminal tombstone; group calls cannot start or ring and a live one ends. Not unit-tested: the owner's workflow (`DissolveGroup`, the pending-sends build, `StateGroups`, retry timers) — no harness for `GroupMembershipWorkflow` |
| Mentions (G015, m1–m6) | Wire `text_spans` (type 6); `SendGroupText`; who a mention reaches (`MentionVerdictFor`, held while a group checks, decided on settle); alerts through a mute once a minute unless mentions are muted too (`NoticeAlerts`, `thread_local_prefs.mute_mentions`); both ingest paths notice; the `thread_mentions` marker ("You were mentioned", jump to the oldest); composer `@` picker with public nicknames; transcript highlight checked at display. Lab `muted_group_alerts_on_a_mention` |

## Open (next first)

1. A lab case for a held mention; a click on a context-menu item under Xvfb did nothing (the picker's and the chat menu's) — check on a real desktop.

## Notes for whoever picks this up

- The mention-alert rule reads the roster on arrival; the transcript highlight must re-check at display (a late roster restores highlights).
- `@everyone` eligibility is the owner in `group_metadata.owner_identity`, which follows the signed roster (G012) — do not trust a role field from the message.
- Lab group scenarios: `./scripts/test/pp_local_test.sh run --suite app --no-build` with `PP_APP_LAB_ARGS="--only <scenario>"`.
