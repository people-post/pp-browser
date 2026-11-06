# Group chat — current state

**Last updated:** 2026-11-05 (open groups G017–G023 accepted and revised after review, not built: [PHASES § O](PHASES.md#o--open-groups-g017g023); one PR at the end). Before that: 2026-11-03 (since 2026-10-30: G016 dissolve merged in #425 on 2026-10-31; in-chat search merged in #462 on 2026-11-02). Before that: 2026-10-30 (G015 mentions implemented, m1–m6; signed roster + member check G012 / G013 merged in #411)

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
| Search in a chat (#462) | The "⋯" menu searches inside the open chat; the matching text is marked, not the whole row |

## Open (next first)

1. **Open groups (G017–G022, calls V062)** — accepted and revised 2026-11-05; **o1 built** (roles, the preset in the owner's statement, `@everyone` for admins, Make admin / Remove as admin, lab `group_owner_appoints_an_admin`; the preset's switch waits for o4). **o3 built** on its own branch (`feat/group-call-make-group`, 2026-11-05): "Make a group…" in a grown call, invites by account with `context: call`, the in-call banner, and the G022 consent amendment (a stranger's invite from the live call we are both in passes `contacts_only`); lab `call_grows_into_a_group` written, not yet run. **Guests in a group's call (V062) built** on `feat/call-guests` (2026-11-05): brought per preset, labelled, removed by the owner and admins, "Add {name} to the group" from the owner until o4; lab `group_call_guest` written, not yet run. Next: o2. Order: o1 roles and presets → o2 changes on top of the owner's statement → o3 make a group during a call → o4 member invites and removal → o5 invite links; guests in a group's call after o1 and o4. Changes are signed by the device key, the owner's statement by the account key (G018 item 9, decided 2026-11-05). o2 also takes the group id off the relay (G018 item 7); message requests (G023) belong to the privacy project. [PHASES § O](PHASES.md#o--open-groups-g017g023).
2. A lab case for a held mention; a click on a context-menu item under Xvfb did nothing (the picker's and the chat menu's) — check on a real desktop.

## Notes for whoever picks this up

- The mention-alert rule reads the roster on arrival; the transcript highlight must re-check at display (a late roster restores highlights).
- `@everyone` eligibility is the owner in `group_metadata.owner_identity`, which follows the signed roster (G012) — do not trust a role field from the message. The UI (`ChatPresenter`, `PeoplePickerController`) still decides the owner by roster role where `IsLocalOwner` is not asked, while the engine checks `metadata.owner_identity`.
- Lab group scenarios: `./scripts/test/pp_local_test.sh run --suite app --no-build` with `PP_APP_LAB_ARGS="--only <scenario>"`.
