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
**Status:** Proposed (Kenneth, 2026-10-25) — needs Hongwei's decision  
**Decision:** `group_targets` gets `restart_pending` (additive column, same pattern as the `chat_targets` column of 3cf0a14c8). Set on restore from the local account backup, and on accepting a **rejoin invite**: the invitee cannot tell from local state that it was a member before (it lost the group), but the owner can, so a `group_invite` to an identity already on the owner's roster carries `rejoin: true` and that member's `sender_epoch_floor`. The next group send uses epoch = max(`sender_epoch_floor` from the owner's statement or rejoin invite (G012), the highest `seen_epoch_from_you` in roster replies (G013), the backup's epoch, the locally persisted max) + 1, seq 1, and clears the flag. No wall clock. Receivers need no change: `chat_target_epoch` of a group envelope is the envelope's own epoch (`RelayReceivePipeline.cpp:1185`), so a never-used epoch starts a fresh sync state and is accepted.  
**Rationale:** Today AcceptInvite seeds `group_targets` with (1, 1) (`GroupMembershipWorkflow.cpp:342`, upsert, so an existing row is overwritten too), and a restored device has no seq. It numbers from 1 in its old epoch, and peers drop those messages as "another message holds this seq (a sender rewound within the epoch)" (`E2eIngestClassifier.cpp`, SilentDiscard, relaxed branch). Same mechanism as the 1:1 bug fixed in 3cf0a14c8, which covers Direct threads only. A wall-clock epoch would let clock skew push the epoch into the future for good, and would show members when the device was restored.  
**Alternatives:** Receiver accepts an envelope flagged "sender restart" as a new sequence (the sender need not know any old epoch; costs a signed envelope flag and receiver state reset, and a replayed flagged envelope must not reset anything).

---

## G011 — Group epoch decoupled from the pair session epoch

**Date:** 2026-10-25  
**Status:** Proposed (Kenneth, 2026-10-25) — needs Hongwei's decision  
**Decision:** Group sends use the pair's current PSK session. The group epoch in the envelope is used only for receiver de-dup and ordering, not to select or create a pair session. Suspected, not yet proven: first pin it with a test (restore, then send 1:1 then group, and the reverse; assert both delivered and the pair epoch never decreases).  
**Rationale:** `GroupE2ePayloadCodec.cpp:39` resolves the pairwise master PSK with `ResolveMasterPskForEpoch(target_key, group_epoch)`, which matches only the current or retired epochs. On a miss it opens a new key_init session and saves it with `record.session_epoch = group epoch` (`:67`). After a restore, where the group epoch (G010: higher or lower than the pair's) differs from the pair's epoch (bumped by 3cf0a14c8), this could write a lower epoch into the pair record and rewind the 1:1 chat.  
**Alternatives:** Keep one shared epoch and make the group bump also bump the pair (couples two sequences, more key_init churn); give groups their own PSK table keyed by (peer, group_id).

---

## G012 — Owner-signed roster statement; any online member can relay it

**Date:** 2026-10-25  
**Status:** Proposed (Kenneth, 2026-10-25) — needs Hongwei's decision  
**Decision:** On every roster change the owner signs a full `roster_statement` (members with role and joined_at, `sender_epoch_floor` per member, `roster_epoch`, owner signature) and sends it with the roster event. Every member stores the latest one. Wire in DESIGN.md "Restore and roster sync".  
**Rationale:** Today only `member_joined` can carry a roster (optional `members`, a snapshot for the newcomer); `member_left`, `member_removed`, `owner_transferred`, `group_renamed` carry only the changed identity and `roster_epoch`. A restored device therefore cannot learn the current roster from any single event, and nobody can prove what the roster is except the owner. The signed statement is verifiable offline from any member, and its epoch floors feed G010.  
**Alternatives:** Owner-only answers (a restored member waits for the owner to be online); replay the event log (long, needs history); unsigned rosters from members (a member could forge adds or removals).

---

## G013 — Roster sync after restore: group_roster_request / group_roster_reply

**Date:** 2026-10-25  
**Status:** Proposed (Kenneth, 2026-10-25) — needs Hongwei's decision  
**Decision:** A restored group where we are not the owner enters `syncing`: send `group_roster_request` to the owner and all members; any member replies with its stored owner-signed statement. The requester verifies the owner signature, takes the highest `roster_epoch`; the owner's own reply wins when it arrives. Sync completes on a valid reply to this `request_id`, not through `RequireNewerEpoch` (`GroupMembershipApply.cpp:105` rejects `inbound <= current`, so an unchanged roster, same epoch, would be dropped as stale). While syncing: receive yes, send no. `removed=true` marks the local group "you were removed", read-only. Owner's restored groups do not sync (the owner signs every change and the backup is rewritten on each). Re-invited groups do not sync (the `member_joined` snapshot is fresh).  
**Rationale:** The backup roster can be stale. Sending while syncing would encrypt to members removed after the backup (a leak) or leave the group looking joined while peers drop our messages.  
**Alternatives:** Trust the backup roster (leak above); sync only from the owner (blocks while the owner is offline); reuse `member_joined` snapshots (needs the owner to re-add us).

---

## G014 — Group size limit 50 and refused accept

**Date:** 2026-10-25  
**Status:** Proposed (Kenneth, 2026-10-25) — needs Hongwei's decision  
**Decision:** Limit 50, counting the active roster only (not pending invites). The owner refuses create/invite beyond 50. An invite sent while not full but accepted when full: the owner refuses the accept and sends `group_invite_refused` `{ group_id, invite_nonce, reason: "group_full" }`, so the invitee does not believe it joined. Kenneth decided 50.  
**Rationale:** Send cost is N pairwise ciphertexts per message (G008) and the `member_joined` snapshot carries every member's card; both grow with size. Without a refusal message the invitee, who creates its local group on accept, would show a group the owner never added.  
**Alternatives:** No limit; count pending invites against the limit (blocks invites that may never be accepted).
