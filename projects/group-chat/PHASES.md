# Group chat — phases

Ordering and checkboxes only. **Status:** [CURRENT_STATE.md](CURRENT_STATE.md). **Spec:** [DESIGN.md](DESIGN.md), decisions [DECISIONS.md](DECISIONS.md); normative wire / disk in [WIRE_SCHEMAS](../../docs/contracts/WIRE_SCHEMAS.md) and [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md).

```
C0 ── C1 ── C2 ── C3 ── C4 ── C5            (base group chat — landed)
                 └── R (signed roster + member check, G010–G013 — landed)
                       └── M (mentions, G015: m1 ── m2 ── m3 / m4 ── m5 ── m6)
L (size limit, G014) — independent
O (open groups, G017–G023): o1 ── o2 ── o4 ── o5
                            o3 (any time; needs only today's create-group path)
                            o6 (privacy; o6c with o2's ops in the plaintext)
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

Accepted 2026-11-05 (Hongwei). One PR for the whole feature, at the end. Each phase: gtests for its rules, a lab case where two or more devices are needed, then promote the shipped wire to WIRE_SCHEMAS / DATA_LAYOUT and set the decision's status.

**o1 — Roles and the signed policy (G017)**
- [ ] `admin` role live; the permission table as one function every check calls (actor, action, roster, policy)
- [ ] Policy keys and defaults (`invite`, `links`, `call_guests`, `rename`, `history`); owner / admins change it; UI in the group's settings
- [ ] `@everyone` for the owner and admins (G015 amendment): eligibility, the composer offer, the transcript highlight
- [ ] Appoint / demote an admin (owner), shown in the member list

**o2 — The signed change log (G018)**
- [ ] Decide which key signs an op from a linked device (G018 Open) — before any wire
- [ ] Op codec and signature (`roster_op`), op id; the roster computed from heads with R1–R4; checkpoint
- [ ] Ops replace the owner statement on every roster event (`member_joined`, `member_left`, `member_removed`, `owner_transferred`, `group_renamed`, `group_dissolved`); member check (G013) carries ops
- [ ] Heads in each group copy's plaintext; catch-up on a mismatch
- [ ] gtests: every merge rule (concurrent remove vs add, demote vs the demoted admin's removal, concurrent renames, adds past 50); lab: two admins acting while the owner is offline

**o3 — Make a group during a call (G022)**
- [ ] "Make a group…" on a call with no group and 3+ people; picker prefilled with the participants
- [ ] The invite with `context: call` shown as a banner in that call (Join / Not now); the live call stays as it is
- [ ] Lab: a 1:1 call grows to three, one makes a group, two join, one says not now and stays in the call

**o4 — Any member invites; removal and bans (G019, G020)**
- [ ] Invites by members per policy; `approval`: suggestions to admins; accept goes to the inviter, who signs the `add`
- [ ] Invite shows name and member count only; Accept states what joining shares; "Alice added Carol" and "added by" in the member list; 10 invites per group per 24 h in the inviter's client
- [ ] Removal by admins (members only); order (remaining members first); the removed notice; "They keep the messages they already received"; ban / unban
- [ ] Lab: a member invites with the owner offline; an admin removes and bans, the banned account cannot be re-invited

**o5 — Invite links (G021)**
- [ ] `link_add` / `link_revoke` ops; the link format; QR through the scan flow
- [ ] Join request → preview → confirm; refusals; 20 requests an hour per link
- [ ] Lab: join by link; a revoked and an expired link refused

**o6 — Privacy (G023)**
- [ ] o6a newcomers see messages from the join (`history: since_join`); `full` by policy
- [ ] o6b message requests for direct messages from group members who are not contacts (the requests list P007 left open)
- [ ] o6c the group id and roster heads move inside the copy's ciphertext; the relay envelope of a group copy looks like a direct one; WIRE_SCHEMAS updated

**Calls — guests in a group's call ([calls V062](../p2p-av-calls/DECISIONS.md#v062--guests-in-a-groups-call-brought-by-the-groups-policy-removal-by-owner-and-admins))**, with o1 (policy) and o4 (roles in use)
- [ ] `guest` / `brought_by` on the mid-call invite in a group's call; "joined as …'s guest", the Guest label
- [ ] `call_guests` policy checked by each participant's device
- [ ] `call_remove` from the owner / admins; key rotates without them; their device leaves
- [ ] "Add {name} to the group" for a guest; the guest's "Joining shows you to N people"
- [ ] CALLS.md updated when it ships
