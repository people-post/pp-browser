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
