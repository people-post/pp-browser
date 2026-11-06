# Group chat — phases

Ordering and checkboxes only. **Status:** [CURRENT_STATE.md](CURRENT_STATE.md). **Spec:** [DESIGN.md](DESIGN.md), decisions [DECISIONS.md](DECISIONS.md); normative wire / disk in [WIRE_SCHEMAS](../../docs/contracts/WIRE_SCHEMAS.md) and [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md).

```
C0 ── C1 ── C2 ── C3 ── C4 ── C5            (base group chat — landed)
                 └── R (signed roster + member check, G010–G013 — landed)
                       └── M (mentions, G015: m1 ── m2 ── m3 / m4 ── m5 ── m6)
L (size limit, G014) — independent
O (open groups, G017–G022, calls V062): o1 ── o2 ── o4 ── o5
                                         o3 (any time; needs only today's create-group path)
                                         guests (after o1 and o4)
```

## C0–C5 — Base group chat

- [x] C0 `e2e_public` auto-key send / compose enablement
- [x] C1 `route.kind=group`, N ciphertexts per message (G008)
- [x] C2 Membership events: invite / accept / decline / remove / leave / owner transfer / rename / fork
- [x] C3 Roster storage (`GroupRosterStore`), group-scoped sync
- [x] C4 UI: create group, roster, invite cards, invite policy
- [x] C5 Fork with members, fresh start (G002)

## R — Signed roster and member check (G010–G013)

- [x] G010 / G011 per-pair numbering in groups
- [x] G012 owner-signed roster statement with an owner chain (`GroupRosterStatement`), re-signed after a leave, carried by roster events
- [x] G013 member check (`GroupRosterSync`, request / reply on the control lane — `MakeGroupRosterSyncFamily`), send gate while checking / removed
- [x] Groups and card floors in the local account backup; lab `group_restore_checks_members`, `group_removed_member_restores`, `group_restored_owner_takes_newer_roster`
- Restore itself is unsupported in the product ([at-rest A011](../at-rest-crypto/DECISIONS.md)): the member check only runs in the lab until restore returns

## L — Group size limit (G014)

- [x] Owner refuses create / invite beyond 50 active members (`kMaxGroupMembers`, `kGroupFullError`)
- [x] `group_invite_refused { group_id, invite_nonce, reason: "group_full" }` for an accept that arrives when full (`ApplyInviteAcceptToRoster` closes the invite; `GroupMembershipWorkflow::RefuseInvite`); the invitee drops its local group (`RelayReceivePipeline::ApplyInviteRefused`: removed, read-only, a line saying why) — only for its own invite from that sender
- [x] Wire doc (WIRE_SCHEMAS § Group size); gtests (apply, ingest both sides, codec). No lab case: it needs 51 users

## M — Mentions (G015)

**m1 — Wire**
- [x] `ChatPayload` content type 6 `text_spans` (`lib/messaging/TextSpans.*`, `ChatPayloadCodec`): structure checks reject, a bad range drops its span, unknown kinds kept and written back; stored as `Text` with `spans` in `payload_json`
- [x] Unit tests (`text_spans_test.cpp`)

**m2 — Who a mention reaches; alerts**
- [x] `MessageMentionsUs` (`lib/messaging/MentionAlert.*`): our account mentioned, or everyone from the signed roster's owner; never own / blocked sender / direct chat / removed / checking group
- [x] Background notices carry `mentions_us`; a muted group alerts on a mention at most once a minute per group (`ChatController`, `kMutedMentionAlertGapMs`)
- [x] **Held mentions:** `MentionVerdictFor` tells a mention that cannot be decided yet (`Held`: the group is checking, or its roster unknown) from one that does not reach us; the orchestrator holds it in memory and decides it when the group's roster changes (`GroupRosterStore::SetOnChanged` → `DecideHeldMentions`) — marked and alerted like a fresh one if under `kHeldMentionMaxAgeMs` (10 min), else dropped. gtest for the verdict; no lab case yet (a restore's check settles too fast to mention into reliably)
- [x] Alerts for group messages that arrive over Amp too: both ingest paths raise the background notice through one helper (`MeshDeliveryOrchestrator::BackgroundNoticeFor`); the alert decision is one function (`NoticeAlerts`, used by `ChatController` and the probe)

**m3 — Mute mentions too**
- [x] `thread_local_prefs.mute_mentions` column (additive, added on open; `ThreadLocalPrefsStore`), read in the `SetOnBackgroundUnread` handler; cleared on unmute
- [x] Menu choice on a muted group: "Mute mentions too" / "Alert on mentions" (`SessionMenuItem::MuteMentions` / `AllowMentions`, `ChatPresenter::SetMuteMentions`), en + zh-Hans

**m4 — Mention marker**
- [x] `thread_mentions` rows (`ThreadMentionsStore`, DATA_LAYOUT): written by both live ingest paths (`NoteMention`; not when the chat is open; never by history sync), dropped on thread open, on message delete (for everyone or here), with the thread; gtest `thread_mentions_store_test`; lab `muted_group_alerts_on_a_mention` checks the marker comes and goes
- [x] Chat list row flag `ChatSessionRow::mentioned` (`ConversationsFacade::ThreadsWithMentions`), `OldestMention(thread)` for the jump
- [x] Chat list shows "You were mentioned" (`sidebar.rml`, `.sidebar-session-mention`); opening jumps to the oldest unread mention (`ChatPresenter::OpenedAtMention`, `ChatTranscriptScroller::JumpToMessage`)

**m5 — Sending and display**
- [x] `ConversationsFacade::SendGroupText(thread, text, spans)`: spans checked, kept in `payload_json`; a group send with spans carries the full ChatPayload (type 6) — it used to send only rich non-text content that way
- [x] Composer: `@` opens a member picker (`ChatMentionPicker`; at the start, after whitespace or non-ASCII text); inserts `@` + the member's **public nickname** (`ConversationsFacade::KeptCardNickname`, else the contact's directory nickname, else a short id) — never the private remark; owner sees `@everyone`; spans recomputed from the final text at send (`MentionSpansForDraft`)
- [x] Transcript: eligible spans highlighted (`EligibleMentionRuns`: members of the roster now, `@everyone` from its owner; UTF-8 byte ranges; links win); tap opens the member's contact
- [x] Re-encode rule: quote / share build fresh plain text (no spans carried); a quoted part's spans are never highlighted or matched at send

**m6 — Proof and promotion**
- [x] Probe `send_mentions`, `mute_mentions` / `allow_mentions`, a `notice` event (kind message: `mentions_us`, `alerts`); lab `muted_group_alerts_on_a_mention` (a plain message silent, a mention alerts, a member's `@everyone` does not, mentions muted too: silent)
- [x] Promote: WIRE_SCHEMAS normative; DATA_LAYOUT `thread_mentions` / `mute_mentions` lines match the code; G015 status → implemented. gtests `group_mentions_test`, `transcript_mentions_test`. Seen on screen 2026-10-30 (the GUI on a lab member's profile, Xvfb): "You were mentioned" in the list, the jump, highlights for a member's mention and the owner's `@everyone`, the picker (members by public name, no `@everyone` for a member), Tab choosing; a picker that did not reopen after a send was fixed. A click on any context-menu item (the picker's, and the older chat menu's alike) did nothing under Xvfb — to check on a real desktop

## O — Open groups (G017–G023)

Accepted 2026-11-05 (Hongwei), revised the same day after review (statement plus changes instead of a general log; two presets; no bans, approval, invite count or history setting). One PR for the whole feature, at the end. Each phase: gtests for its rules, a lab case where two or more devices are needed, then promote the shipped wire to WIRE_SCHEMAS / DATA_LAYOUT and set the decision's status. Not in O: message requests (G023 item 3) — privacy project. The relay's view of groups is in o2 (G018 item 7).

**o1 — Roles and presets (G017)**
- [x] `admin` role live; one permission function every check calls (`GroupAllows`, `lib/messaging/GroupPermissions.h`); the unused `GroupPermission` flags and `invite_policy` / `history_visibility` fields removed
- [x] Presets Open (default) / Managed in the owner's statement (sign bytes v2, clean cutover; `group_metadata.policy_json` `{preset}`); receivers take roles and preset on keep and adopt; the owner sets it (`SetGroupPreset`, probe `set_group_preset`) — **its switch in the group's settings comes with o4**: until members' invites and renames carry on the wire, "members add people" would not be true
- [x] `@everyone` for the owner and admins (G015 amendment): eligibility (`MentionVerdictFor`), the composer offer and the send-time check, the transcript highlight (`MentionRoster::everyone_senders`)
- [x] Appoint / demote an admin (owner): `member_role_changed` / `group_preset_changed` with the re-signed roster, refused unless the roster bears them out; "Make admin" / "Remove as admin" in the member menu and the person's group-admin card; the Admin badge in the member list and group info; the group chat's line
- [x] gtests (`group_permissions_test`, the statement's preset and admins, `GroupSettingIngestTest`, mention alert / roster, member tap); lab `group_owner_appoints_an_admin`

**o2 — Changes on top of the owner's statement (G018)**
- [x] **Engine, a pure library (not wired yet):** `GroupChange` (codec, device-key sign / verify against the signer's card, change id, `ChangesHash`), the statement's v3 fields and sign bytes (`added_by`, title, `removed`, `links`, `folded`; `SignStatement`), `CheckChanges` / `ComputeRoster` (R1–R6, any order the same roster) and `FoldChanges`, in `lib/messaging/GroupChange.*`, `GroupRosterRules.*`, `GroupRosterStatement.*`; encodings in [DESIGN § Open groups](DESIGN.md#open-groups-g017g023). gtests `group_change_test` (signature, tampering, cards, golden ids, statement v3) and `group_roster_rules_test` (each rule's G018 scenario, presets, links, fold-then-compute, all 40320 orders of eight changes). Still to do for the items below: wiring into the workflow, the receive pipeline, the member check and the UI
- [ ] Changes signed by the device key and checked against the signer's card; the statement stays account-signed (G018 item 9); a change carries the signer's card when the receiver may lack it
- [ ] Change codec and signature; the roster from the latest statement plus changes by R1–R6; the owner folds changes into its next statement
- [ ] Statement fields: roles, preset, `added_by`, `removed`, `links`, `folded`
- [x] The relay stops learning the group (G018 item 7): the group id inside each copy's ciphertext (off `route.group_id`; sign v4), the member check just after decryption, stream id `v1:g:` + hash(group id, the pair) + the sending device (no epoch: a record holds copies at several pair epochs, M022); WIRE_SCHEMAS / MESSAGE_ENCRYPTION updated; gtests `GroupCopyPrivacyTest`, `RelayStreamKeyTest`, `ACopysPlaintextNamesItsGroup`, `AMissingGroupCopyIsAskedForInItsGroup`
- [ ] `statement_epoch` and `changes_hash` in each group copy's plaintext; the member check (G013) carries the statement and changes; catch-up on a mismatch
- [ ] gtests: each rule (a removal against a concurrent member's add; a re-admit; a demoted admin's backdated removal void; a removed inviter's adds kept by the remover; concurrent renames; adds past 50; a change from a device taken off the card void unless folded; any order of the same changes gives the same roster); lab: a member invites and an admin removes while the owner is offline, the owner folds when back

**o3 — Make a group during a call (G022)**
- [ ] "Make a group…" on a call with no group and 3+ people; picker prefilled with the participants
- [ ] The invite with `context: call` shown as a banner in that call (Join / Not now); the live call stays as it is
- [ ] Lab: a 1:1 call grows to three, one makes a group, two join, one says not now and stays in the call

**o4 — Members invite; removal (G019, G020)**
- [ ] Invites by members per preset; accept goes to the inviter, who signs the `add`
- [ ] Invite shows name and member count only; Accept states what joining shares; "Alice added Carol" and "added by" in the member list
- [ ] Removal by admins (members only); order (remaining members first); the removed notice; "They keep the messages they already received"; re-admit only by owner / admins
- [ ] Bulk: "Remove everyone {name} added", "Re-invite everyone {name} removed"
- [ ] Lab: a member invites with the owner offline; an admin removes, a member's re-invite of that account is void, an admin re-admits

**o5 — Invite links (G021)**
- [ ] `link_add` / `link_revoke`; links die with their maker's membership; the link format; QR through the scan flow
- [ ] Join request → preview → confirm; refusals; 20 requests an hour per link
- [ ] Lab: join by link; a revoked, an expired, and a removed maker's link refused

**Guests in a group's call ([calls V062](../p2p-av-calls/DECISIONS.md#v062--guests-in-a-groups-call-labelled-brought-by-members-removed-by-owner-and-admins))** — after o1 and o4
- [ ] `guest` / `brought_by` on the mid-call invite in a group's call; "joined as …'s guest", the Guest label
- [ ] Only members allowed to invite bring guests; each participant's device checks
- [ ] `call_remove` for guests from the owner / admins; key rotates without them; their device leaves
- [ ] "Add {name} to the group" for a guest; the guest's "Joining shows you to N people"
- [ ] CALLS.md updated when it ships
