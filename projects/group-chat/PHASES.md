# Group chat — phases

Ordering and checkboxes only. **Status:** [CURRENT_STATE.md](CURRENT_STATE.md). **Spec:** [DESIGN.md](DESIGN.md), decisions [DECISIONS.md](DECISIONS.md); normative wire / disk in [WIRE_SCHEMAS](../../docs/contracts/WIRE_SCHEMAS.md) and [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md).

```
C0 ── C1 ── C2 ── C3 ── C4 ── C5            (base group chat — landed)
                 └── R (signed roster + member check, G010–G013 — landed)
                       └── M (mentions, G015: m1 ── m2 ── m3 / m4 ── m5 ── m6)
L (size limit, G014) — independent
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

## L — Group size limit (G014, proposed)

- [ ] Owner refuses create / invite beyond 50 active members
- [ ] `group_invite_refused { group_id, invite_nonce, reason: "group_full" }` for an accept that arrives when full; the invitee drops its local group
- [ ] Wire doc + lab case

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
- [ ] Chat list shows "You were mentioned"; opening jumps to the oldest unread mention (UI)

**m5 — Sending and display**
- [x] `ConversationsFacade::SendGroupText(thread, text, spans)`: spans checked, kept in `payload_json`; a group send with spans carries the full ChatPayload (type 6) — it used to send only rich non-text content that way
- [ ] Composer: `@` opens a member picker; inserts `@` + the member's **public nickname** (their card's, else the roster's) — never the user's private remark; owner sees `@everyone`
- [ ] Transcript: highlight eligible spans (member / owner check at display, so a late roster restores them); tap opens the member
- [ ] Re-encode rule: same chat keeps spans; any future forward / quote drops mention and everyone spans

**m6 — Proof and promotion**
- [x] Probe `send_mentions`, `mute_mentions` / `allow_mentions`, a `notice` event (kind message: `mentions_us`, `alerts`); lab `muted_group_alerts_on_a_mention` (a plain message silent, a mention alerts, a member's `@everyone` does not, mentions muted too: silent)
- [ ] Promote: WIRE_SCHEMAS already normative; DATA_LAYOUT `thread_mentions` / `mute_mentions` lines describe m3 / m4 — confirm against the code when they land; G015 status → implemented
