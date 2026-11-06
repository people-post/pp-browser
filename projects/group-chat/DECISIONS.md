# Group chat — decisions log

Cross-project refs: [D076/D089/D095](../chat-storage-and-memory/DECISIONS.md), [E022](../e2e-message-crypto/DECISIONS.md).

---

## G001 — Invite authority: owner-only (v1), role-extensible schema

**Date:** 2026-07-16  
**Status:** Amended by [G017](#g017--open-groups-roles-and-two-presets) / [G019](#g019--members-invite-the-invitee-consents-who-brought-whom-is-shown) (accepted 2026-11-05, not built): admins exist, and any member may invite in an Open group  
**Decision:** v1 enforces **owner-only** invite, remove, rename, and ownership transfer. Wire and storage include `MemberRole` (`owner` | `admin` | `member`), `PermissionFlags`, and `group_policy.invite_policy` (`owner_only` | `admins` | `any_member`) so v1.1 can enable admins or any-member invites without a wire bump.  
**Rationale:** Simple spam surface for v1; avoid second migration when roles expand.  
**Alternatives:** Any-member invites in v1 (rejected — spam); hard-coded owner-only without role fields (rejected — blocks v1.1).

---

## G002 — Fork history: fresh start (v1), copy-ready wire

**Date:** 2026-07-16  
**Decision:** v1 forks always create a new `group_id` with an **empty transcript**. `group_forked` events carry `history_mode` (`fresh` | `copy_to_fork_point` | `user_selected`), optional `source_group_id`, and optional `fork_message_id` for future copy. `IThreadStore::ExportMessagesUpTo` is the v1.1 hook.  
**Rationale:** Lowest complexity for first ship; same event type supports copy later.  
**Alternatives:** Copy history in v1 (deferred); fork without audit event (rejected).

---

## G003 — New member history: full backfill on join

**Date:** 2026-07-16  
**Status:** Amended by [G023](#g023--privacy-in-open-groups-history-from-the-join-the-relays-view-comes-later) (accepted 2026-11-05): a newcomer sees messages from the join; the full backfill below was never built, and a full-history option waits for backfill  
**Decision:** Accepted members receive **full group history** via peer backfill. `history_visibility` on group metadata defaults to `full`; `since_join` reserved for private-group mode.  
**Rationale:** Matches common chat UX.  
**Alternatives:** Join-point-only history in v1 (deferred via metadata field).

---

## G004 — Group identity: locally generated `group:<uuid>`

**Date:** 2026-07-16  
**Decision:** Creator generates UUID locally; wire format `group:<uuid>`. First creator is initial owner. No relay-assigned group ids.  
**Rationale:** P2P-first; no relay dependency for identity.  
**Alternatives:** Relay-assigned ids (rejected).

---

## G005 — Offline invites: outbox queue

**Date:** 2026-07-16  
**Decision:** Invites to offline peers reuse the existing **outbox** pattern with retry until delivered or expired.  
**Rationale:** Consistent with direct messaging delivery.  
**Alternatives:** Drop invite if peer offline (rejected).

---

## G006 — Membership authority vs message sync

**Date:** 2026-07-16  
**Status:** Amended by [G018](#g018--members-and-admins-sign-roster-changes-on-top-of-the-owners-statement) / [G019](#g019--members-invite-the-invitee-consents-who-brought-whom-is-shown) (accepted 2026-11-05, not built): the member who sent the invite commits the add as a signed op; the owner is no longer the only one  
**Decision:** **Owner-signed roster events** are authoritative for membership. **Per-sender `sender_seq`** + relaxed ingest (D046) handles message divergence — not owner arbitration. `session_epoch` bumps on membership/crypto changes.  
**Rationale:** Aligns with D089 tier design; avoids single-writer bottleneck for messages.  
**Alternatives:** Owner as message sync authority (rejected).

**Invite commit (clarification):** A pending invite is not membership. `group_invite_accept` is a request the owner validates against the pending row; the peer-facing commit is owner-signed `member_joined` (monotonic `roster_epoch`). Group encrypt targets are active roster members only.

---

## G007 — Invite spam controls (three layers)

**Date:** 2026-07-16  
**Decision:** (1) Per-invite Decline / Block inviter; (2) `TrustLevel::Blocked` enforcement at ingest; (3) profile setting `group_invite_policy`: `everyone` | `contacts_only` | `nobody` plus rate limits on pending invites.  
**Rationale:** Layered defense without blocking all DMs from a contact.  
**Alternatives:** Contact block only (insufficient for group spam).

---

## G008 — Group wire: N ciphertexts per member (D095)

**Date:** 2026-07-16  
**Decision:** Group outbound uses `body.e2e.member_payloads`: map of recipient communicating identity → base64 AEAD blob. Pairwise secrets reuse `ChatTargetKey` machinery (E022).  
**Rationale:** Resolved O008/D095; reuses 1:1 crypto.  
**Alternatives:** Single group PSK (rejected — E022); MLS v1 (deferred).

---

## G009 — Owner leave uses transfer+leave; prune is owner-choice

**Date:** 2026-07-24  
**Status:** (2) amended by [G020](#g020--removing-a-member-order-notice-re-admit) (accepted 2026-11-05, not built): admins may remove members too  
**Decision:** (1) Owner leave of a multi-member group fans out a single `owner_transferred` DM with `leave_previous: true` so peers apply succession and remove the old owner atomically. Plain `member_left` from a recorded owner is rejected. Solo-owner close is local dismiss only. (2) Send/encrypt failure marks a member unreachable locally; only the owner may `member_removed` after explicit confirm — no auto-drop. (3) Unreachable owner surfaces a local in-chat advisory (Fork / Message owner / Got it); no automatic second owner.  
**Rationale:** Avoids transfer-without-leave / leave-without-transfer split-brain; keeps prune under human control; Fork is the ownership escape hatch.  
**Alternatives:** Separate transfer then leave DMs; auto-promote next member; auto-prune on N failures.

---

## G010 — A sender that restarts in a group starts a never-used epoch

**Date:** 2026-10-25  
**Status:** Accepted (Hongwei, 2026-10-25) — **superseded in mechanism by the per-pair numbering amendment below** (the goal stands: a restarted sender is never taken for one already held)  
**Decision:** `group_targets` gets `restart_pending` (additive column, same pattern as the `chat_targets` column of 3cf0a14c8). Set on restore from the local account backup, and on accepting a **rejoin invite**: the invitee cannot tell from local state that it was a member before (it lost the group), but the owner can, so a `group_invite` to an identity already on the owner's roster carries `rejoin: true` and that member's `sender_epoch_floor`. The next group send uses epoch = max(`sender_epoch_floor` from the owner's statement or rejoin invite (G012), the highest `seen_epoch_from_you` in roster replies (G013), the backup's epoch, the locally persisted max) + 1, seq 1, and clears the flag. No wall clock. Receivers need no change: `chat_target_epoch` of a group envelope is the envelope's own epoch (`RelayReceivePipeline.cpp:1185`), so a never-used epoch starts a fresh sync state and is accepted.  
**Rationale:** Today AcceptInvite seeds `group_targets` with (1, 1) (`GroupMembershipWorkflow.cpp:342`, upsert, so an existing row is overwritten too), and a restored device has no seq. It numbers from 1 in its old epoch, and peers drop those messages as "another message holds this seq (a sender rewound within the epoch)" (`E2eIngestClassifier.cpp`, SilentDiscard, relaxed branch). Same mechanism as the 1:1 bug fixed in 3cf0a14c8, which covers Direct threads only. A wall-clock epoch would let clock skew push the epoch into the future for good, and would show members when the device was restored.  
**Alternatives:** Receiver accepts an envelope flagged "sender restart" as a new sequence (the sender need not know any old epoch; costs a signed envelope flag and receiver state reset, and a replayed flagged envelope must not reset anything).
**Accepted amendment (Hongwei, 2026-10-25):** the floor arrives with the member check (G013): the owner's `sender_epoch_floor` in the statement and each replier's `seen_epoch_from_you`, so a restored device learns it before its first send at no extra round trip. The receiver-side "sender restart" flag is rejected: a replayed flagged envelope would have to be told apart from a real restart.
**Superseding amendment — numbering per pair (Hongwei, 2026-10-25):** a member only ever receives its own copy of a group message, so a copy is numbered in the pair: `session_epoch` is the pair's e2e_public session epoch and `sender_seq` the sender's own count for this group in that pair (`group_member_seq`, kept across leave and re-join). The key is the pair's session at that epoch. A device that lost its counts is the pair's restart (D014, 3cf0a14c8): the first send to that member, 1:1 or group copy, moves the pair to its next epoch from seq 1. No group epoch, no floors, no `rejoin` flag, no own epoch in the backup; a member removal bumps nothing. Old and new apps keep reading each other's group copies (the envelope epoch is the pair's, which is what old receivers look the key up by). Supersedes the floors above and the `key_epoch` / sign v3 of the G011 amendment (built, then withdrawn on this branch before merge). Two linked devices of one account: the pair becomes a device pair ([M021](../multi-device-account/DECISIONS.md#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits)) — one copy per member device, numbered in that device pair.

---

## G011 — Group epoch decoupled from the pair session epoch

**Date:** 2026-10-25  
**Status:** Accepted (Hongwei, 2026-10-25) — **implemented** with numbering per pair (see the G010 amendment); the `key_epoch` mechanism below was withdrawn  
**Decision:** Group sends use the pair's current PSK session. The group epoch in the envelope is used only for receiver de-dup and ordering, not to select or create a pair session. Suspected, not yet proven: first pin it with a test (restore, then send 1:1 then group, and the reverse; assert both delivered and the pair epoch never decreases).  
**Rationale:** `GroupE2ePayloadCodec.cpp:39` resolves the pairwise master PSK with `ResolveMasterPskForEpoch(target_key, group_epoch)`, which matches only the current or retired epochs. On a miss it opens a new key_init session and saves it with `record.session_epoch = group epoch` (`:67`). After a restore, where the group epoch (G010: higher or lower than the pair's) differs from the pair's epoch (bumped by 3cf0a14c8), this could write a lower epoch into the pair record and rewind the 1:1 chat.  
**Alternatives:** Keep one shared epoch and make the group bump also bump the pair (couples two sequences, more key_init churn); give groups their own PSK table keyed by (peer, group_id).
**Accepted amendment (Hongwei, 2026-10-25):** confirmed in code — on a miss `EncryptForMembers` saves a new session into the pair's `chat_targets` row with `session_epoch = group epoch`, overwriting the 1:1 session, and the 1:1 restart of 3cf0a14c8 makes the two epochs differ more often. The group's message numbering keeps its own epoch; a group send uses the pair's current session (opening one at the pair's own epoch when there is none) and never writes the group epoch into the pair row.
**How, first built (withdrawn — see the G010 per-pair amendment):** the group envelope names the pair's session epoch in a signed `key_epoch` (sign v3; MESSAGE_ENCRYPTION § Canonical relay-envelope signing bytes). Each member's copy is exactly a pairwise `e2e_public` message at that epoch, opened and announced as a 1:1 send does; the receiver reads and keeps it as one (`ReconcileE2ePublicSession`). Chosen over trial decryption (no wire change, but the receiver could not tell which pair epoch a group-carried opener belonged to). Clean cutover: group messages between old and new apps are refused. Also found and fixed by it: after any member removal (which moves the group epoch) the owner's second message and every other member's failed ("Missing key_init_b64"); lab `group_after_member_removed`.

---

## G012 — Owner-signed roster statement; any online member can relay it

**Date:** 2026-10-25  
**Status:** Accepted (Hongwei, 2026-10-25), amended: the statement carries the member list only (no `sender_epoch_floor` — numbering is per pair, G010) — **extended by [G018](#g018--members-and-admins-sign-roster-changes-on-top-of-the-owners-statement)** (accepted 2026-11-05, not built): the statement stays the base; members and admins sign changes on top  
**Decision:** On every roster change the owner signs a full `roster_statement` (members with role and joined_at, `sender_epoch_floor` per member, `roster_epoch`, owner signature) and sends it with the roster event. Every member stores the latest one. Wire in DESIGN.md "Restore and roster sync".  
**Rationale:** Today only `member_joined` can carry a roster (optional `members`, a snapshot for the newcomer); `member_left`, `member_removed`, `owner_transferred`, `group_renamed` carry only the changed identity and `roster_epoch`. A restored device therefore cannot learn the current roster from any single event, and nobody can prove what the roster is except the owner. The signed statement is verifiable offline from any member, and its epoch floors feed G010.  
**Alternatives:** Owner-only answers (a restored member waits for the owner to be online); replay the event log (long, needs history); unsigned rosters from members (a member could forge adds or removals).
**Accepted amendments (Hongwei, 2026-10-29) — implemented:** (1) **Account key, self-certifying.** The owner signs with its account ML-DSA key and the statement carries that key: its account id is derived from it, so any member can hand the statement on and the receiver checks it offline. (2) **Owner chain.** A transfer is signed by the owner giving the group up (`owner_transfer` in `owner_transferred`); statements carry every transfer since the group began, so a device that knew an earlier owner (restored from an older backup) trusts its successor, and a former owner cannot outbid its successor (a statement signed by an owner a chain shows had already handed the group on loses). The new owner signs the roster once the transfer reaches it. (3) **After a leave the owner re-signs.** `member_left` is the leaver's, so it cannot carry the owner's statement: the owner, applying it, sends `group_roster_statement` to the members. Wire: [WIRE_SCHEMAS § Owner-signed roster](../../docs/contracts/WIRE_SCHEMAS.md) (superseded here).

---

## G013 — Roster sync after restore: group_roster_request / group_roster_reply

**Date:** 2026-10-25  
**Status:** Accepted (Hongwei, 2026-10-25), with the amendments below; replies carry no `seen_epoch_from_you` (numbering is per pair, G010) — the check is only that the member list is current before sending  
**Decision:** A restored group where we are not the owner enters `syncing`: send `group_roster_request` to the owner and all members; any member replies with its stored owner-signed statement. The requester verifies the owner signature, takes the highest `roster_epoch`; the owner's own reply wins when it arrives. Sync completes on a valid reply to this `request_id`, not through `RequireNewerEpoch` (`GroupMembershipApply.cpp:105` rejects `inbound <= current`, so an unchanged roster, same epoch, would be dropped as stale). While syncing: receive yes, send no. `removed=true` marks the local group "you were removed", read-only. Owner's restored groups do not sync (the owner signs every change and the backup is rewritten on each). Re-invited groups do not sync (the `member_joined` snapshot is fresh).  
**Rationale:** The backup roster can be stale. Sending while syncing would encrypt to members removed after the backup (a leak) or leave the group looking joined while peers drop our messages.  
**Alternatives:** Trust the backup roster (leak above); sync only from the owner (blocks while the owner is offline); reuse `member_joined` snapshots (needs the owner to re-add us).
**Accepted amendments (Hongwei, 2026-10-25):** (1) **The owner checks too.** A restored owner asks like anyone else and adopts a newer statement it signed that a member holds — an owner restored from a backup written just before a removal would otherwise put the removed member back. (2) **Any incoming group message also starts the check** when one is not running: its sender is online to answer. (3) History recovery is out of scope — a later feature for 1:1 and groups alike (ask a peer; the peer chooses).
**Implemented (2026-10-29):** the latest request id stays open, so a later reply still applies when it wins (the owner's); a check unanswered for 15 s is asked again when a group message arrives, and every group still checking asks again when messaging comes up (the app quit mid-check). Request and reply ride the control lane (`MakeGroupRosterSyncFamily`): a restored device keeps none of the account cards it had, and a member's reply as a device copy (no `key_init`, so no `sender_card`) could not be verified there. A member still checking its own roster answers with its statement only (no `removed`, its roster may be stale); without a statement only the owner's `removed` counts. Groups ride in the local account backup as an optional `groups[]` in the sealed contents (no version bump; [DATA_LAYOUT § Local account backup](../../docs/contracts/DATA_LAYOUT.md#local-account-backup-ios-keychain-outside-the-data-dir)), rewritten on any roster change. *Later:* restore is unsupported in the product ([at-rest A011](../at-rest-crypto/DECISIONS.md#a011--local-account-backup-and-restore-unsupported-in-the-product)); groups will ride in the account locker instead ([M023](../multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold)) — the member check itself is unchanged. Lab: `group_restore_checks_members`, `group_removed_member_restores`, `group_restored_owner_takes_newer_roster`. **Amended (2026-11-04):** a check still unanswered is also asked again on a timer, after 15 s, 60 s and 5 min (`GroupMembershipWorkflow::kRosterReaskDelaysMs`; each re-ask only while that check still waits, `group_roster_sync::StillAwaiting`). The NAT lab (`restore_from_phrase`) showed a quiet group stuck checking, unable to send: the restored device asked before its new device list reached its peers, they answered sealed for the device they knew, and the control lane does not resend; with no member writing, nothing asked again.

---

## G014 — Group size limit 50 and refused accept

**Date:** 2026-10-25  
**Status:** Accepted (Hongwei, 2026-10-30); **implemented** 2026-10-30 — `kMaxGroupMembers`, the refusal in `ApplyInviteAcceptToRoster`, `group_invite_refused` ([WIRE_SCHEMAS § Group size](../../docs/contracts/WIRE_SCHEMAS.md)). The invitee keeps the group's chat, read-only (sync state removed) with a line saying why, rather than deleting it.  
**Decision:** Limit 50, counting the active roster only (not pending invites). The owner refuses create/invite beyond 50. An invite sent while not full but accepted when full: the owner refuses the accept and sends `group_invite_refused` `{ group_id, invite_nonce, reason: "group_full" }`, so the invitee does not believe it joined. Kenneth decided 50.  
**Rationale:** Send cost is N pairwise ciphertexts per message (G008) and the `member_joined` snapshot carries every member's card; both grow with size. Without a refusal message the invitee, who creates its local group on accept, would show a group the owner never added.  
**Alternatives:** No limit; count pending invites against the limit (blocks invites that may never be accepted).

---

## G015 — Mentions in group messages

**Date:** 2026-10-30  
**Status:** Accepted (Hongwei, 2026-10-30), with the review amendments folded in (generic spans, composer names, held notifications, per-message marker, mute level, stated limits); **implemented** 2026-10-30 (phases m1–m6, [PHASES](PHASES.md)). The composer also opens the picker after non-ASCII text (a Chinese draft has no space before `@`). — `@everyone` **amended by [G017](#g017--open-groups-roles-and-two-presets)** (implemented 2026-11-05): the owner and admins  
**Decision:**

| Part | Rule |
|---|---|
| Wire | New `ChatPayload` content type **6 `text_spans`**: the text, then up to 64 spans, each `kind`, a UTF-8 byte range and a length-prefixed body. Kinds today: **1 mention** (body: the member's `account_id`) and **2 everyone**. A kind a build does not know is kept and ignored, so later ones (links, a reply quote, formatting) need no new type. Layout: [WIRE_SCHEMAS § text_spans](../../docs/contracts/WIRE_SCHEMAS.md). A message with no span stays type 0. Mentions name **account ids**, never nicknames. |
| Composer | Inserts `@` + the member's **public nickname** (their account card's, or the roster's) — never the sender's private remark for that contact, which every member would see. |
| Local | Stored as `ChatContentType::Text`; the spans ride in `payload_json`. Re-encoded as received for the same chat (unknown kinds included); a copy into another chat (a future forward / quote) drops mention and everyone spans. |
| `@everyone` | Only the group owner may use it: the owner the signed roster names (G012), so a former owner whose handover we hold cannot. |
| Receive | Structure errors reject the message (including more than 50 mentions, one account mentioned twice, two everyone spans). A bad range drops only that span (text shown normally). Membership of `account_id` and the owner check on everyone are made against the local roster **when displayed or when notifying**, not at decode. Mentions count in group threads only; in a direct chat the text shows plain. |
| Notify | Not muted: as for any group message. Muted and mentioned: notify **at most once per group per 60 s** — unless the user muted mentions too (below). Muted and not mentioned: silent, as today. Never for a mention from ourselves or from a contact we blocked, nor in a group that is **removed** (G013). In a group still **checking** its members (G013), or before its roster is known, a mention is **held**: decided when the check settles if the message is under 10 min old, else dropped. Messages from history sync never mark or notify. |
| Mute | Muting a group gets a choice: mentions still alert (default) or **mute mentions too** (`thread_local_prefs.mute_mentions`, [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md)). |
| Marker | Per message, not per thread: `thread_mentions` rows in `profile.db` for the unread live messages that mention us — the chat list shows "You were mentioned" and opening can jump to the oldest; deleting the message (for everyone or here) drops its row, so a deleted mention leaves no marker. Opening the thread drops them all. |
| Limits | At most 50 mentions per message (G014), 64 spans. All devices on the same build: unreleased, no backward compatibility; an older build shows "update to view". |

**Rationale:** A mention bypasses mute, so it must not rest on a nickname the receiver guesses (duplicate names, renames, a member who picks another's name). Account ids plus byte ranges let the receiver highlight exactly what the sender inserted; the public nickname keeps a private remark private. Spans with a kind and a skippable body make this the last text content type: what comes next is a kind, readable by older builds as plain text. Eligibility is not decided at decode because the roster can arrive after the message (a restored device, G013; a new member; a fresh owner transfer): a late roster then restores the highlights, and holding notifications while the roster is being checked keeps a restored device from either missing or misjudging them. A per-message marker stays right when a mention is deleted and lets the UI jump to it. The 60 s limit stops one member from flooding a muted group with mentions; muting mentions too is the user's way out of even that; history sync is excluded so a backlog cannot raise old alerts.  
**Stated limits:** (1) Each member gets its own ciphertext (G008), so a sender can give different members different spans — e.g. tell one member alone they are mentioned. Low harm; not detected. (2) The marker is per device: opening the chat on one device leaves the others' (no read state between devices yet). (3) A mention held past 10 min, or one whose roster never settles, is never notified (it still shows highlighted).  
**Alternatives:** Match `@nickname` in the text at the receiver (misattribution, bypasses mute on a guess); a mention-only content type (every later text feature costs another type and an "update to view"); put spans in a JSON tail (JSON is forbidden in signed/hashed bytes, WIRE_SCHEMAS); check membership at decode (drops highlights when the roster is late); decide notifications once on arrival (a restored or new member misses them); a per-thread flag (stays set after the mentioning message is deleted); let any member use `@everyone` (spam of a whole group); no way to mute mentions (a muted group can still ring every 60 s).  
**Cross-link:** [WIRE_SCHEMAS § text_spans](../../docs/contracts/WIRE_SCHEMAS.md), [DATA_LAYOUT § profile.db](../../docs/contracts/DATA_LAYOUT.md), G008 (per-member ciphertexts), G012 (owner-signed roster), G013 (roster check), G014 (size limit 50).

---

## G016 — The owner dissolves a group for everyone

**Date:** 2026-10-31
**Status:** Accepted (Kenneth, 2026-10-31); Hongwei may amend; **implemented** 2026-10-31 (the marker is `group_metadata.sync_state` `dissolved`, no new column; wire in [WIRE_SCHEMAS § Group dissolved](../../docs/contracts/WIRE_SCHEMAS.md))
**Decision:** The owner's choice is "dissolve the group", which ends it for every member. It replaces "dismiss on this device only" (G009) for a group with other members; leaving with a transfer stays as it is.

1. **Wire:** new membership `control_type` `group_dissolved`, sent by the owner as a DM to every roster member and carrying the owner-signed roster statement form (G012) with the group id and a `dissolved` marker, so a receiver checks it the same way as other owner events.
2. **Receiver:** only from the owner the signed roster names, in a group it is in. It empties the roster and marks the group dissolved. **The thread and its messages stay**, read-only, with one line: "{owner} dissolved this group". The composer is off; the user removes the thread when they want to. Late or repeated copies do nothing. Traffic for a dissolved group is dropped. An unanswered invite to it is void.
3. **Offline members** get it from the inbox like other membership events. A member who missed it and sends again gets no delivery from peers who dissolved; the inbox delivers the dissolve when they come back, and that is enough.
4. **Owner:** confirm once with plain wording, then fan out and remove locally. If some sends fail, the rest still go; retries use the outbox.
5. **Solo owner:** same choice, nothing to send.

**Rationale:** On device test, an owner used "dismiss on this device only". The owner was removed from its own roster and its thread was deleted, nothing was sent, and the other members kept fanning out to it, so the group was left with no owner and silent drops. Kenneth: dissolving ends the group for everyone; each member keeps the chat with a line saying so, and deletes it themselves.
**Alternatives:** Keep the local dismiss (the case above); transfer only (a group can never end while one other member remains, and that member becomes owner unasked).
**Old apps:** an app before this does not know `group_dissolved`: it stores a plain system line in its direct chat with the owner (the DM's `display` text) and its group stays live for it; its sends are dropped by every app that dissolved, and it is told nothing more. The product is unreleased, so no shim.
**Tombstone:** none. The kept thread and the dissolved marker on the group are enough; a member who deleted the chat has no thread, so later traffic is rejected as for any unknown group.


---

## G017 — Open groups: roles and two presets

**Date:** 2026-11-05
**Status:** Accepted (Hongwei, 2026-11-05) — **o1 implemented 2026-11-05** (roles, the preset in the owner's statement, `@everyone` for admins: [WIRE_SCHEMAS § Owner-signed roster](../../docs/contracts/WIRE_SCHEMAS.md)); the preset takes effect with o4 (members invite and rename). Amends G001 (owner-only invites) and G015 (`@everyone` owner-only). Phases [PHASES § O](PHASES.md#o--open-groups-g017g023).
**Decision:** Groups are **open by invite**: members may bring people in, and the owner and admins keep order. There is no group directory or public join (open by invite only, for now).

1. **Roles.** `owner` (one), `admin` (any number, appointed and demoted by the owner), `member`. Roles live in the owner's signed statement (G012, G018).
2. **Two presets, set by the owner:**

| Preset | Invite, make invite links (G021), bring guests into the group's call ([calls V062](../p2p-av-calls/DECISIONS.md#v062--guests-in-a-groups-call-labelled-brought-by-members-removed-by-owner-and-admins)) | Rename |
|---|---|---|
| **Open** (default) | any member | any member |
| **Managed** | admins and the owner | admins and the owner |

3. **The rest is fixed:**

| Action | Who |
|---|---|
| Send | everyone in the group |
| Remove a member (G020); remove a guest from the group's call | the owner and admins (admins: members only, not admins) |
| Re-admit someone removed (G020) | the owner and admins |
| `@everyone` | the owner and admins |
| Appoint / demote admins, set the preset, transfer (G009), dissolve (G016) | the owner |

4. **Every client enforces.** A receiver checks each change against the owner's statement it holds (G018) and drops what is not allowed. Each member gets its own copy of every message (G008), so a client that breaks the rules can still reach people one at a time; conforming clients do not show or apply what it sends.

**Rationale:** The product owner wants openness — anyone can bring their friends — with order kept by people the group trusts rather than a gatekeeper. Two presets cover the groups we expect (a circle of friends, a group someone runs) and stay explainable; separate knobs per action multiply the cases to build, test and explain. `@everyone` stays with the owner and admins in every group: it is the one thing that cuts through a mute (G015).
**Alternatives:** Owner-only invites (G001: every invite waits for one person); one setting per action (G017 as first written: 144 combinations); any member may also remove (a group could empty itself in a quarrel).

---

## G018 — Members and admins sign roster changes on top of the owner's statement

**Date:** 2026-11-05
**Status:** Accepted (Hongwei, 2026-11-05) — item 7 built (2026-11-05, [WIRE_SCHEMAS § Group](../../docs/contracts/WIRE_SCHEMAS.md)): the stream id has no epoch and keeps the sending device (`v1:g:{hash}:from:{device}`: since M022 a record holds copies at several pair epochs); the rest not built; phases [PHASES § O](PHASES.md#o--open-groups-g017g023). Extends G012 (the owner's statement stays the base) and amends G006 (members and admins commit their own changes). Wire sketch: [DESIGN § Open groups](DESIGN.md#open-groups-g017g023).
**Decision:**

1. **The owner's statement stays the authority** (G012 as built: signed, linear by `roster_epoch`, with the owner chain). It holds the members, roles, the preset, the title, the removed list (below), live invite links, and the changes it folded in.
2. **Changes on top**, each signed by the **device** that makes it and checked against its account's card (item 9):

| Change | Signer |
|---|---|
| `add` (an account, with its invite or link) | per preset (G017); a re-admit: the owner or an admin |
| `leave` | the one leaving (not the owner: G009) |
| `remove` | the owner; an admin for members |
| `rename` (with a `title_version`) | per preset |
| `link_add` / `link_revoke` (G021) | per preset / the link's maker, admins, owner |

3. **Changes apply in any order with the same result**, so members agree without anyone ordering them:
   - **R1 — add and leave combine.** An account is in the group if an `add` of it stands and no `leave` or `remove` names that add.
   - **R2 — removal wins.** A `remove` of X names the adds of X it saw, and also voids any *member's* `add` of X it did not see. After a removal, only the owner or an admin brings X back, with an `add` that names the removal (re-admit). The statement keeps removed accounts in its removed list, so this holds after folding.
   - **R3 — a change counts while its signer may make it.** A change is valid if its signer may make it under the **latest owner statement this device holds** — not the one it was made under, so a demoted admin's backdated change gets nowhere. A signer's changes still count after the signer lost the right if something entitled kept them: the owner's statement folded them in, a `remove` of the signer lists them (the remover's device keeps the people they brought in), or the signer's own `leave` lists them.
   - **R4 — renames:** the highest `title_version`, then the higher role, then the lower change id.
   - **R5 — size:** adds beyond 50 (G014), in change-id order, are void; whoever signed one sends its invitee `group_invite_refused`.
   - **R6 — a device off the card.** A change signed by a device that the signer's newest card (the one this device holds) no longer lists counts only if it was folded or kept (R3). A device taken out of an account stops being able to act in its groups as soon as members hold the newer card.
4. **The owner folds** the changes it holds into its next statement when its device is online. The statement names what it folded, and devices drop those. Nothing needs the owner online: changes stand on their own until folded.
5. **Keeping members in step.** Each group copy carries, inside its ciphertext, the sender's statement epoch and a hash of the changes it holds. A receiver that differs asks for what it lacks on the control lane (the G013 request / reply, carrying changes).
6. **Newcomers check everything.** They get the owner's statement and every unfolded change, all signed: nobody, the member who added them included, can hand them a roster that is not the group's.
7. **The relay stops learning which group a copy is for (moved here from G023, 2026-11-05).** Today the relay sees each group copy's group id twice: `route.group_id` on the envelope (signed, in the clear) and the stream id `v1:group:{group_id}:{epoch}:{member}`, so it can map who is in which group and how busy it is. It needs neither — messenger treats the stream id as an opaque key and never reads the route. With o2, which already changes what each group copy carries inside its ciphertext (item 5):
   - **The group id moves inside each copy's ciphertext.** The copy is already authenticated, so it stays bound to its group; the receiver reads the group id after decrypting. The member check that rejects a non-member's copy moves from before decryption to just after it (one pairwise decrypt from a known sender).
   - **The stream id becomes** `v1:g:` + a hash of (group id, epoch, the two account ids). Both accounts and all their devices compute it, so history fetches keep working; the relay cannot (the group id is a 122-bit random value it never sees), and with the pair in the hash, Alice→Bob and Alice→Carol in one group look unrelated.
   - **Still visible:** that a record is a group copy (the `v1:g:` prefix), and N same-size copies at one moment (padding and jitter, later — G023).
   - **Cutover:** old and new clients cannot read each other's group copies, and group history stored on the relay under the old stream ids is not fetched again. Acceptable while the product is unreleased.
8. **Clean cutover** (unreleased product).
9. **Which key signs (decided 2026-11-05, Hongwei).** Two keys, by what the act can do:

| Signed with | Acts | From |
|---|---|---|
| The **device key**, checked against the account's card as messages are ([multi-device M021](../multi-device-account/DECISIONS.md#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) §7) | every change: `add`, `leave`, `remove`, `rename`, `link_add`, `link_revoke` — all a member or admin does | any linked device |
| The **account key** | the owner's statement: appointing / demoting admins, the preset, folding changes in, `transfer`, `dissolve` (G009, G016) | today every linked device (all hold the account key); once M021 takes it off non-master devices, the master |

   A change names its signer's account and device and carries the signer's card when the receiver may not hold it (a change by someone who has since left, in a sync reply); the card proves itself (the account id derives from the account key that signs it).

**Stated limits:** (1) A change by someone the owner demotes or removes at the same moment, and that nobody kept, is dropped — even a removal. The owner chose to revoke them; what was dropped can be redone. (2) A stolen device can act in its account's groups — invite, remove — until the account takes it off its card and members hold the newer card (M021 limit 4); the remedy is G020's bulk actions. Until M021 takes the account key off non-master devices, a stolen device holds the account key too, so for an owner it can also sign the statement; option C limits the damage only from then.
**Rationale:** Any-member invites (G017) cannot each wait for the owner's device. The first version of this entry made the roster a general log of changes that named the changes before them, merged by rank; review found it hard to build and to keep correct, with two holes: a demoted admin could sign removals that claimed to precede the demotion and every member would keep them, and a newcomer started from a snapshot signed by whoever added them, who could forge it. Keeping the owner's statement as the base and allowing only changes that combine in any order closes both — R3 checks every change against the latest statement, and a newcomer verifies everything — and leaves set operations to build and test instead of a merge of branches.
**Signing keys, rationale:** Everyday openness must work from the phone in someone's hand, so changes use the device key, the path messages already take. The acts that cannot be undone — handing the group over, dissolving it, changing who is in charge — stay behind the account key, which M021 means to keep on one device; a stolen phone then cannot seize a group. Account key for everything (rejected: members could invite only from the master once M021 lands, and a stolen device could sign anything as the account until the key — and with it the account id — changes); device key for everything (rejected: a stolen phone could dissolve or transfer an owner's group, and the statement would have to carry the owner's card to stay checkable offline).
**Alternatives:** A general log with merge rules (the first version: the holes above); admins only propose and the owner's device commits (the owner stays a bottleneck); MLS (a group key would cost G008's property that a removal takes effect at once).

---

## G019 — Members invite; the invitee consents; who brought whom is shown

**Date:** 2026-11-05
**Status:** Accepted (Hongwei, 2026-11-05) — not built; phases [PHASES § O](PHASES.md#o--open-groups-g017g023). Amends G001 and G006 (the invite is accepted by the member who sent it, not only the owner).
**Decision:**

1. **Who may invite:** per preset (G017). In a Managed group a member asks an admin in the chat; there is no suggestion protocol.
2. **The invitee consents, as today.** The invite is a DM from the inviter; the invitee's own `group_invite_policy` applies (default `contacts_only`, G007): a friend's invite reaches you, a stranger's does not. Accept goes back to the inviter, whose device signs the `add` (G018) and sends it with the newcomer's card to the members, and the statement, the changes and the members' cards to the newcomer, as `member_joined` does now.
3. **Before accepting, the invitee sees** the inviter, the group's name and its member count — **not the member list**. Members' cards and names reach only someone who joined; a decline learns no one's keys (as now).
4. **Accept states what joining shares:** "Joining shares your profile with N people."
5. **Accountability.** The chat shows "Alice added Carol"; the member list shows who brought each member. Removing an inviter keeps the people they brought (R3).
6. **The limit is the size limit** (G014). No per-member invite count: a modified client would skip it; the remedy for a flood is G020's "Remove everyone Alice added".

**Rationale:** Openness on the group side and consent on the person's side: anyone can bring a friend, nobody is put in a group they did not accept. Showing who added whom keeps an open group accountable without a gatekeeper. Hiding the member list until a join keeps members from being shown to everyone who is merely invited.
**Alternatives:** Owner-only invites (G001); an approval step for members' invites (a protocol and a UI for what asking in the chat already does); auto-join without accepting (shares a profile without consent); show the member list on the invite (exposes members to every invitee).

---

## G020 — Removing a member: order, notice, re-admit

**Date:** 2026-11-05
**Status:** Accepted (Hongwei, 2026-11-05) — not built; phases [PHASES § O](PHASES.md#o--open-groups-g017g023). Extends G009 (2): admins may remove members too.
**Decision:**

1. **Who:** the owner removes anyone but itself; an admin removes members, not admins or the owner (G017).
2. **Order.** The `remove` goes to the remaining members first, then to the person removed. Until a member applies it, that member still sends the removed person copies; sending to the people who stay first shortens that window, and the state carried on every message (G018 5) closes it.
3. **The removed person is told:** "You were removed by Alice". Their chat stays, read-only (the removed state of G013). The real enforcement is the other members' clients: they stop sending copies, so a removed device that ignores the notice still gets nothing new.
4. **Honest copy.** Removal says "They keep the messages they already received."
5. **Re-admit, no ban list.** Someone removed can be brought back only by the owner or an admin (G018 R2); a member's invite of them is void. That keeps a friend from re-adding them a minute later without a ban list to keep.
6. **Bulk remedies in the member list** (owner and admins): **"Remove everyone Alice added"** — for a member who filled the group with throwaway accounts; **"Re-invite everyone Bob removed"** — after an admin removed people they should not have (the owner demotes Bob; what Bob removed and the owner had not folded is void already, G018 R3; the rest are re-invited, and each accepts again).
7. **Calls.** Someone removed from the group drops out of the group's live call (calls V053, V058), unchanged.

**Stated limits:** Someone removed can come back under a new account through a member's invite; in an Open group only switching to Managed stops that (a ban list would not either). A stolen owner account has the whole group, dissolve included; that is the account's recovery to solve (recovery phrase, linked devices), not the group's.
**Rationale:** Pairwise copies (G008) make a removal take effect for content as soon as each member applies it, with no group key to rotate; what is left is the window before members hear of it, which ordering and the carried state shorten. Telling the person is honest: starving them silently is noticed anyway. Re-admit by admins replaces ban / unban with one rule. The bulk actions are the human remedy for the abuse an open group invites, one tap instead of forty.
**Alternatives:** Remove silently; any member removes (G017); a ban list with unban (more state and changes, and new accounts get around it anyway); removing the people an inviter brought along with them (punishes people for who invited them).

---

## G021 — Invite links

**Date:** 2026-11-05
**Status:** Accepted (Hongwei, 2026-11-05) — not built; phases [PHASES § O](PHASES.md#o--open-groups-g017g023). Wire sketch: [DESIGN § Open groups](DESIGN.md#open-groups-g017g023).
**Decision:**

1. **Who may make one:** per preset (G017).
2. **A link carries** a random link id and secret, its expiry, and the account of the member who made it — **not** the group's id, name or members. A `link_add` change (G018) puts the link id, a hash of the secret, the expiry and an optional use limit in the roster, so every admin knows it and can revoke it (`link_revoke`). Default expiry: 7 days.
3. **A link lives while its maker is in the group:** when the maker leaves or is removed, their links stop working, whatever their expiry.
4. **Joining.** Opening the link sends a join request (link id, secret) to its maker. The maker's device checks the secret, the expiry, the uses, revocation, the removed list (G020) and the size limit; only then does it show the group's name and member count, and on the joiner's confirm it signs the `add`. If the maker is offline the request can go to an admin, who has the link from the roster.
5. **Rate.** A link's answerer takes at most 20 join requests an hour for it.
6. **No server.** Nothing resolves links; a link is only bytes between people, shown as text or a QR code (the scan flow, [SCANNING.md](../../docs/ui/SCANNING.md)).

**Stated limits:** Whoever holds a link learns the account of the member who made it. A leaked link lets strangers join until it expires or is revoked; admins remove (G020).
**Rationale:** Bringing a friend should not need the friend to be your contact first. A link that names neither the group nor its members leaks nothing to someone who only sees it, and keeping links in the signed roster lets any admin see and revoke them.
**Alternatives:** Links through a server (a lookup service that sees every join); links that name the group and preview it without the secret (leaks the group to anyone who sees the link); approval for every link join (friction in an open group; a Managed group already limits who makes links).

---

## G022 — Make a group during a call

**Date:** 2026-11-05
**Status:** Accepted (Hongwei, 2026-11-05) — not built; phases [PHASES § O](PHASES.md#o--open-groups-g017g023).
**Decision:**

1. **Where:** a call not tied to a group (one that started 1:1 and grew; `origin_group_id` empty), once three or more people are in it. The call screen offers **"Make a group…"**.
2. **Who:** any participant. They become the new group's owner.
3. **How:** the people picker opens with everyone in the call selected (each can be unticked) and a default name from the participants' names; confirming creates the group and invites each selected participant — the existing create-group path ("Add people" from a direct chat), with the call's participants prefilled.
4. **The invitees** see the invite as a banner in the call: "Alice made a group "{name}" with everyone here — Join / Not now". It is the ordinary invite: nobody is added without Join (G019).
5. **The live call is left alone.** It does not become the group's call: group calls drop people who are not in the group (V053), and someone who chose "Not now" must not lose the call. The next call from the group is a group call.
6. **The new group starts empty**, with one line in its chat: "Group made during a call". The earlier direct chat between the first two stays between them.

**Rationale:** People often grow a 1:1 call into a small group call and then want to keep talking as a group. Turning the call into an invite with one tap keeps the consent of each person and reuses what exists.
**Alternatives:** Add everyone automatically (no consent; a group lasts and shares profiles, a call does not); attach the live call to the new group (drops whoever has not joined yet).

---

## G023 — Privacy in open groups: history from the join; the relay's view comes later

**Date:** 2026-11-05
**Status:** Accepted (Hongwei, 2026-11-05). Item 1 needs no work; item 2 moved into O (G018 item 7); item 3 is a follow-up outside O ([privacy CURRENT_STATE](../privacy/CURRENT_STATE.md)). Amends G003 (full backfill on join).
**Decision:**

1. **A newcomer sees messages from when they joined.** G003's full backfill was never built (there is no `history_visibility` in the code), so this is what happens today; it becomes the rule, with no setting. In an open group people come in whom the earlier members never picked, and what members wrote was shared with the people present then. A group option for full history waits for history backfill (a later feature for 1:1 and groups alike, G013).
2. **The relay's view of groups — moved into O (G018 item 7, 2026-11-05).** A closer look found it needs no relay change (messenger treats stream ids as opaque and never reads the route), and o2 already changes what each group copy carries inside its ciphertext, so it ships with o2 as one cutover for group copies.
3. **Follow-up — message requests** for direct messages from people who are not contacts, group members included: an open group hands your card, and its relay route, to people you do not know. [Privacy P007](../privacy/DECISIONS.md#p007--who-can-call-me-strangers-messages-stay-open-for-now) left strangers' messages open until a requests list exists; that list is privacy work, not O. Until then, Block is the remedy.

**Later:** padding and send jitter, so N copies of one message at the same moment, of the same size, do not stand out.
**Rationale:** Openness is about who can come in, not about what was said before they came or what the relay can see. Item 1 costs nothing; item 3 is work other features share, so it gets its own design rather than riding in this one.
**Alternatives:** Full history by default (shares members' past messages with people they did not pick); fold items 2 and 3 into O (a wire and relay change, and a new inbox, inside a group feature).
