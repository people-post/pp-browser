# Feature / app reorg — phases

**[NORTH_STAR.md](NORTH_STAR.md)** is the working target map.  
**This file orders work only.** Prefer small PRs; each phase must leave includes/CMake/tests green.

## Agent batch delivery

1. Read [CURRENT_STATE.md](CURRENT_STATE.md) + this file; pick the **next unchecked** phase item.
2. Prefer **one peel family** or **one mechanical split** per PR.
3. Mark checkboxes in the same PR that lands the move.
4. If a candidate turns out cross-peer, demote it in [CANDIDATES.md](CANDIDATES.md) and stop — do not force a domain edge.
5. After a structural phase, update SRC_LAYOUT / `src/feature/README.md` / include scripts in the same PR.

---

## f0 — Project bootstrap

- [x] README / NORTH_STAR / PHASES / CURRENT_STATE / DECISIONS / CANDIDATES
- [x] Index in [projects/README.md](../README.md)
- [x] Point SRC_LAYOUT migration step 5 at this project
- [x] Agree working North Star is revisable (method locked in [F001](DECISIONS.md#f001--sure-things-first-revisable-north-star))

## f1 — Safe domain peels (messaging stores / policy)

Single-peer or foundation+common-only. Hub keeps owning `unique_ptr`; only the **definition** moves.

- [x] `SqlitePskSessionStore` → `domain/messaging`
- [x] `CallMediaKeyStore` → `domain/messaging` (flat; [F006](DECISIONS.md#f006--sure-peels-use-existing-domain-peers-no-new-peers) — not `domain/media`)
- [x] `PskSessionCoordinator` → `domain/messaging`
- [x] `PublicPskLockCoordinator` → `domain/messaging`
- [x] `EpochBumpCoordinator` → `domain/messaging`
- [x] Update domain CMake + tests colocation; Hub includes new paths

## f2 — People / mesh pure helpers

Clears sharp `feature/ui` → messaging engine includes.

- [x] `ContactReachability` → `domain/people`
- [x] `PeerBriefRoute` → `domain/people`
- [ ] `ProfileIconFetchUtil` — **stays feature** (HTTP → would be people→net; demoted)
- [x] `MobileEphemeralListenGate` → `domain/mesh/reachability/`
- [x] `PeoplePickerLogic` → `domain/ui` (optional same batch)
- [x] `CallConflictCopy` → `domain/ui` (optional same batch)
- [x] UI includes ports/people/domain only — no `ContactReachability` from messaging

## f3 — Responder / contract tidy

- [x] Peel `ChatHistoryResponder` identity coupling; move → `domain/messaging` (mirror `ChatBlobResponder`)
- [x] `IDirectMessageClient` → `common/chat/` (iface only; Amp impl stays)
- [x] Revisit [CANDIDATES.md](CANDIDATES.md) “needs common peel” list; promote any newly unblocked items
  - No promotions this batch; Amp façades / ProfileIconFetchUtil remain blocked/demoted

## f4 — Structural: extract calls (banded first)

Mechanical move after f1–f3. Prefer **nested band** before a new library (avoids messaging↔calls cycle via MeshMessagingService).

- [ ] ADR F004: nested `feature/messaging/calls/` (same `pp_feature_messaging`) — no new target yet
- [ ] Move CallStack / CSM / Lifecycle / Topology / CallUiBackend / CallFunctionalPorts / Amp call façades into band
- [ ] Update includes only; `check_feature_includes.sh` unchanged for module edges
- [ ] Update NORTH_STAR + SRC_LAYOUT feature table if names lock
- [ ] (Later) top-level `pp_feature_calls` only after MeshMessagingService port breaks the cycle

## f5 — Structural: split shell / contacts from `feature/ui`

- [ ] Extract shell host + chrome sync (+ optional `feature/shell`)
- [ ] Extract contacts + people-picker (+ optional `feature/contacts`)
- [ ] Consolidate settings UI sections toward `feature/settings` where cheap
- [ ] Update ports homes only as needed; prefer move-with-owner over new ports
- [ ] ADR lock for folder names; promote to SRC_LAYOUT / feature README

## f6 — App wirers + soft edges (ongoing / last)

- [ ] Split `Application::Initialize` into named wirers (no behavior change)
- [ ] Soften messaging→ai via inbound agent port if touching MessageRouter anyway
- [ ] ChatController / Inbox presentation extraction (highest product risk — last)
- [ ] Demote or schedule remaining cross-peer utils only after `common` contracts exist
- [ ] Archive or freeze this project when layout matches promoted docs

---

## Explicitly deferred

| Item | Why deferred |
|------|----------------|
| `DirectoryShadowCache`, `GroupInviteGate`, `PeerDisplayResolver` | Cross-peer; need common contracts |
| Attachment / registration / directory key *Util* ladders | Feature wiring by nature until peels |
| Amp chat services → mesh | Only if mesh-only after audit |
| Mega “rename everything” PR | Violates sure-things-first |
