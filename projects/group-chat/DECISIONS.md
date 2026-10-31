# Group chat — decisions log

Cross-project refs: [D076/D089/D095](../chat-storage-and-memory/DECISIONS.md), [E022](../e2e-message-crypto/DECISIONS.md).

---

## G001 — Invite authority: owner-only (v1), role-extensible schema

**Date:** 2026-07-16  
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
**Status:** Accepted (Hongwei, 2026-10-25), amended: the statement carries the member list only (no `sender_epoch_floor` — numbering is per pair, G010)  
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
**Implemented (2026-10-29):** the latest request id stays open, so a later reply still applies when it wins (the owner's); a check unanswered for 15 s is asked again when a group message arrives, and every group still checking asks again when messaging comes up (the app quit mid-check). Request and reply ride the control lane (`MakeGroupRosterSyncFamily`): a restored device keeps none of the account cards it had, and a member's reply as a device copy (no `key_init`, so no `sender_card`) could not be verified there. A member still checking its own roster answers with its statement only (no `removed`, its roster may be stale); without a statement only the owner's `removed` counts. Groups ride in the local account backup as an optional `groups[]` in the sealed contents (no version bump; [DATA_LAYOUT § Local account backup](../../docs/contracts/DATA_LAYOUT.md#local-account-backup-ios-keychain-outside-the-data-dir)), rewritten on any roster change. *Later:* restore is unsupported in the product ([at-rest A011](../at-rest-crypto/DECISIONS.md#a011--local-account-backup-and-restore-unsupported-in-the-product)); groups will ride in the account locker instead ([M023](../multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold)) — the member check itself is unchanged. Lab: `group_restore_checks_members`, `group_removed_member_restores`, `group_restored_owner_takes_newer_roster`.

---

## G014 — Group size limit 50 and refused accept

**Date:** 2026-10-25  
**Status:** Proposed — a separate change (not part of the restore work)  
**Decision:** Limit 50, counting the active roster only (not pending invites). The owner refuses create/invite beyond 50. An invite sent while not full but accepted when full: the owner refuses the accept and sends `group_invite_refused` `{ group_id, invite_nonce, reason: "group_full" }`, so the invitee does not believe it joined. Kenneth decided 50.  
**Rationale:** Send cost is N pairwise ciphertexts per message (G008) and the `member_joined` snapshot carries every member's card; both grow with size. Without a refusal message the invitee, who creates its local group on accept, would show a group the owner never added.  
**Alternatives:** No limit; count pending invites against the limit (blocks invites that may never be accepted).

---

## G015 — Mentions in group messages

**Date:** 2026-10-30  
**Status:** Accepted (Hongwei, 2026-10-30), with the review amendments folded in (generic spans, composer names, held notifications, per-message marker, mute level, stated limits); **implemented** 2026-10-30 (phases m1–m6, [PHASES](PHASES.md)). The composer also opens the picker after non-ASCII text (a Chinese draft has no space before `@`).  
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
