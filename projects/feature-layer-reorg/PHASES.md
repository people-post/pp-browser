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

Per [F004](DECISIONS.md#f004--calls-home-nested-band-first-then-top-level) / [F007](DECISIONS.md#f007--vocabulary--end-state-feature-names): call **session** module named `calls`; nest before new lib.

- [x] Nest `feature/calls/` (same `pp_feature_conversations`) — no new target yet
- [x] Move CallStack / CSM / Lifecycle / Topology / CallUiBackend / CallFunctionalPorts / Amp call façades into band
- [x] Update includes only; `check_feature_includes.sh` unchanged for module edges
- [x] Top-level `feature/calls` + `pp_feature_calls` after soft delivery/inbound ports break the CMake cycle
- [x] Rename parent `feature/messaging` → `feature/conversations` (`ConversationsHub` / `ConversationsFacade` / `pp_feature_conversations`)
- [ ] App-owned `CallStack` (hub stops owning call lifecycle) — soft ports already allow one-way `conversations → calls`

## f5 — Structural: split shell / contacts; absorb chat UI

Per [F007](DECISIONS.md#f007--vocabulary--end-state-feature-names): **no top-level `feature/chat`**. Bands are **staging** for the `gui` lift ([F008](DECISIONS.md#f008--gui-layer-above-feature)); do not promote to `pp_feature_shell`.

- [x] Nest shell host + chrome sync under `feature/ui/shell/` (same `pp_feature_ui`)
- [x] Nest contacts + people-picker under `feature/ui/contacts/`
- [x] Move `ChatController` (+ helpers) to `feature/ui/chat/`; retire top-level `feature/chat` + `pp_feature_chat`
- [x] Update `check_feature_includes.sh` (ban retired `feature/chat/` path)
- [ ] Consolidate settings UI sections where cheap (toward `gui/settings` after f7, or keep under residual presenters)
- [ ] Promote remaining SRC_LAYOUT / RUNTIME diagrams after f7 path ships
- [x] ~~(Later) top-level `pp_feature_shell` / `pp_feature_contacts`~~ — **superseded by f7 `src/gui/`**

## f6 — App wirers + soft edges (ongoing)

- [x] Split `Application::Initialize` into named wirers (`WireSettings`, `WireShellPresenters`, `WireCalls`, …) — no behavior change
- [x] Soften conversations→ai via `AgentInboundPorts` (`MessageRouter` / hub / facade; `AgentUiPorts` moved to `feature/ai/`)
- [ ] Inbox presentation extraction (highest product risk — last / after gui lift if cheaper)
- [ ] Demote or schedule remaining cross-peer utils only after `common` contracts exist
- [x] Vocabulary / end-state names locked ([F007](DECISIONS.md#f007--vocabulary--end-state-feature-names))
- [x] GUI layer named `gui` above feature ([F008](DECISIONS.md#f008--gui-layer-above-feature))

## f7 — Lift presenters to `src/gui/` ([F008](DECISIONS.md#f008--gui-layer-above-feature))

Mechanical layer lift; **no** messaging→conversations rename in the same PR.

- [x] ADR already locked ([F008](DECISIONS.md#f008--gui-layer-above-feature)); update NORTH_STAR / SRC_LAYOUT pointers when moving
- [x] Move `feature/ui/**` → `src/gui/**` (keep `shell/`, `contacts/`, `chat/` bands)
- [x] Retire `pp_feature_ui`; add `pp_gui` (single aggregate first)
- [x] Includes: `#include "gui/…"`; app links `pp_gui` above feature
- [x] Guards: ban `feature → gui`; ban retired `feature/ui/` path; `check_gui_includes.sh`
- [x] Update UI_FUNCTIONAL_BOUNDARY / SRC_LAYOUT / `src/feature/README.md` / `src/gui/README.md`
- [ ] (Later) optional bands `gui/call|settings|shared`; split `pp_gui_*` libs if warranted
- [ ] Archive or freeze this project when remaining renames match promoted docs

---

## Explicitly deferred

| Item | Why deferred |
|------|----------------|
| `DirectoryShadowCache`, `GroupInviteGate`, `PeerDisplayResolver` | Cross-peer; need common contracts |
| Attachment / registration / directory key *Util* ladders | Feature wiring by nature until peels |
| Amp chat services → mesh | Only if mesh-only after audit |
| Mega “rename everything” PR | Violates sure-things-first |
| Top-level `pp_feature_shell` / `pp_feature_contacts` | Superseded by `src/gui/` (F008) |
