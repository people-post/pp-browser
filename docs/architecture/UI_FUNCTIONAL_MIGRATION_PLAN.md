# UI ↔ functional migration plan

> **Temporary working doc.** Track phased decoupling of UI presenters from functional singletons.  
> **Delete this file** when all phases below are complete and verified.  
> **Architecture reference (permanent):** [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md)

**Last updated:** 2026-08-03  
**Status:** Phase 1 complete; Phase 2 partial (feedback ports)

---

## How to use this file

1. Work phases **in order** unless a later phase has zero deps on incomplete earlier work.
2. Mark items `[x]` when done; add PR links or commit SHAs in *Notes* if helpful.
3. On resume: search repo for `ShellHost::Instance`, `ChatController::Instance`, and controller `Hub()` — counts should trend down.
4. Do **not** add new `::Instance()` call sites or new controller → controller singleton calls.

---

## Success criteria (global)

- [ ] No functional module writes `ShellHost::State()` or calls another presenter `::Instance()`.
- [ ] Settings, contacts, and people-picker have no `BindMessaging` / `Hub()` on controllers.
- [ ] Chat and shell reach messaging only via facades or app-wired ports.
- [ ] New features follow State / Config / Actions / Events from [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md).
- [ ] `scripts/check_feature_includes.sh` and existing tests pass after each phase.
- [ ] This file deleted.

---

## Phase 0 — Document & guardrails

**Goal:** Team aligned; no new debt.

- [x] Add [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md)
- [x] Add this migration plan
- [x] Link boundary doc from [ARCHITECTURE.md](ARCHITECTURE.md), [RUNTIME_COMPOSITION.md](RUNTIME_COMPOSITION.md), [src/feature/README.md](../../src/feature/README.md)
- [ ] Optional: extend `scripts/check_feature_includes.sh` or a small lint script to flag new `SettingsController::Instance()` from `ShellHost.cpp` (or other forbidden pairs)

**Notes:** `ShellNavigationPorts.h`, `ShellFeedbackPorts.h` added under `src/feature/ui/`.

---

## Phase 1 — Navigation coordinator

**Goal:** Remove direct Shell ↔ Settings (and similar) controller-to-controller singleton calls.

**Primary files:** `ShellHost.cpp`, `SettingsController.cpp`, `FlowCoordinator.*`, `Application.cpp`

- [x] Inventory ShellHost → SettingsController and SettingsController → ShellHost call sites
- [x] Introduce **ShellNavigationPorts** + account-sheet hooks on `ShellHost`:
  - [x] `SetOnAccountSheetOpened` / `SetOnAccountSheetClosed` (app wires → settings)
  - [x] `ShellNavigationPorts` — snapshot, local back, nav tab, panes, dismiss, dirty/sync
  - [x] Documented in [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md) reference table
- [x] Replace bidirectional `::Instance()` calls (ShellHost no longer includes / calls `SettingsController`)
- [x] `SettingsController` no longer `#include`s or calls `ShellHost::Instance()`
- [ ] Verify Me tab open/close, account sheet, and Escape dismiss still work (manual smoke)

**Exit check:** Zero `SettingsController::Instance()` in `ShellHost.cpp` and zero `ShellHost::Instance()` in `SettingsController.cpp` — **done**.

**Notes:** Application fills ports via `bind_shell_presentation_ports()` in `Application.cpp`.

---

## Phase 2 — Extract Actions ports (chat, contacts, shell feedback)

**Goal:** Imperative UI operations go through ports; no new hub pointers on presenters.

**Primary files:** `ChatController.*`, `ContactsController.*`, `PeoplePickerController.*`, `UserFeedback.cpp`, `ShellFeedback.cpp`, `Application.cpp`

- [ ] Define `ChatActions` (or extend ports): send message, select thread internals, agent invoke — narrow args, `Roe<void>` where sync
- [ ] Define `ContactsActions` / extend `ChatSessionPorts` for remaining contacts → chat flows
- [x] Define `ShellFeedbackPorts` + `ShellFeedbackChromePorts` — toast, banner, confirm; `ShellFeedback` / `UserFeedback` no longer call `ShellHost::Instance()` internally
- [x] Wire feedback + navigation ports in `Application.cpp`; clear via `Bind*({})` on shutdown
- [x] Settings confirm/reset dialogs use `ShellFeedbackPorts` (not `ShellHost::State()`)
- [ ] RmlUi callbacks → presenter method → port (no `Hub()` in callback path) — chat/contacts remain

**Exit check:** Feedback helpers (`UserFeedback`, `ShellFeedback` chrome sync) do not call `ShellHost::Instance()` — **done**. Contacts picker / chat callbacks — pending.

---

## Phase 3 — Extract read-only State views

**Goal:** Presenters bind from snapshots, not live mutable service internals.

**Primary files:** `MessagingHub.*`, `ChatController.*`, `ShellHost.*`, `BadgeAggregator.*`, new `*View.h` in `base/` or feature as appropriate

- [ ] `MessagingView` — ready, last error, reachability summary (reuse `SettingsReachabilityView` fields where overlap)
- [ ] `ChatView` — active thread id, unread for thread, composer state (UI-owned draft stays in presenter)
- [ ] `ShellChromeView` — derived chrome for badges (tab unread inputs already partially via `BadgeUnreadInputs`)
- [ ] Presenter `Tick()`: `Snapshot()` → diff → `DataModelHost::Dirty`
- [ ] Stop exposing raw mutable hub state to RmlUi bind paths

**Exit check:** Chat tick does not read messaging internals except through `MessagingView` / facade.

**Notes:**

---

## Phase 4 — Events channel

**Goal:** Functional-initiated UI updates use push, not ad-hoc polling and hidden flags.

**Primary files:** `Application.cpp`, `MessagingHub.*`, presenters, `BadgeAggregator.*`

- [ ] List one-shot events: messaging ready/failed, inbound message, call state, unlock dismissed
- [ ] Add small listener interfaces or app-wired `std::function` hooks on facades
- [ ] Move hub `SetOnMessagingReady`-style wiring entirely into `Application`; presenters refresh via event
- [ ] `BadgeAggregator` uses event-driven refresh where possible instead of every-frame recompute

**Exit check:** No new `SetOn*` on controllers from feature code; callbacks terminate in Application and post to UI thread.

**Notes:**

---

## Phase 5 — Stop functional → shell state writes

**Goal:** Vault, call, and chat logic do not mutate `ShellState` directly.

**Primary files:** `PinGateController.*`, `CallController.*`, `ChatController.*`, `ProfileUnlockGate.*`, `ShellHost.*`

- [ ] PIN gate: all chrome via `ProfileUnlockUiPorts` (already partial) — remove direct `ShellHost::State().pin_gate` writes from non-shell code
- [ ] Call chrome: `CallController` exposes presentation hooks; shell subscribes
- [ ] Chat pane open/close: navigation intents to shell coordinator, not `ShellHost::Instance()` from chat
- [ ] Document remaining **UI-owned** fields in `ShellState` vs presenter-private state

**Exit check:** Grep `ShellHost::Instance().State()` from `feature/chat`, `base/crypto` presentation paths → zero mutation sites outside `feature/ui` shell.

**Notes:**

---

## Phase 6 — Messaging facade (narrow hub exposure)

**Goal:** UI never includes `MessagingHub.h` for orchestration; only facade + views.

**Primary files:** new `MessagingFacade.*` (likely `feature/messaging/`), `ChatController.*`, `ShellHost.*`, `ContactsController.*`, `Application.cpp`

- [ ] Introduce `MessagingFacade` implementing State + Actions (+ event subscription)
- [ ] `Application` owns hub; facade holds reference or pointer to hub internals
- [ ] Remove `BindMessaging` / `Hub()` from chat, shell, contacts controllers
- [ ] Migrate call sites incrementally (chat first, then shell reachability badges, then contacts)

**Exit check:** No `MessagingHub& Hub()` on UI controllers; hub header not included from presenter headers unless unavoidable during tail migration.

**Notes:**

---

## Phase 7 — Agent facade

**Goal:** Chat/settings agent UI uses `AgentFacade` (turn status, invoke, cancel).

**Primary files:** `AgentSession.*`, `ChatController.*`, `ActionRouter.*`, `Application.cpp`

- [ ] `AgentView` — idle / planning / executing / error, visible tool name
- [ ] `AgentActions` — start turn, cancel, configure slice already via bridge
- [ ] Chat presenter uses facade; `ActionRouter` remains app-owned

**Exit check:** Chat does not call `AgentSession` methods directly except through facade.

**Notes:**

---

## Phase 8 — Demote presenter singletons

**Goal:** App-owned presenter instances; RmlUi callbacks use injected pointer.

**Primary files:** `Application.*`, `ShellHost.*`, `ChatController.*`, `SettingsController.*`, `DataModelHost.*`

- [ ] `Application` holds `unique_ptr` (or members) for each presenter
- [ ] Pass presenter reference into RmlUi registration lambdas (app phase after context create)
- [ ] Deprecate `::Instance()`; grep until zero non-test call sites
- [ ] Keep `DataModelHost` as registry singleton **or** move registry to Application — decide in implementation

**Exit check:** `grep -r '::Instance()' src/feature src/app | wc -l` → 0 (excluding tests and explicit migration shims).

**Notes:**

---

## Phase 9 — Cleanup & delete this file

- [ ] Remove deprecated `::Instance()` methods and `BindMessaging` shims
- [ ] Update [RUNTIME_COMPOSITION.md](RUNTIME_COMPOSITION.md) diagrams (dashed Shell ↔ Settings edge removed)
- [ ] Update [src/feature/README.md](../../src/feature/README.md) “Current state” / cycle-breaking table
- [ ] Run full test suite + manual smoke (chat send, settings flush, unlock, call, nav)
- [ ] Delete **UI_FUNCTIONAL_MIGRATION_PLAN.md**
- [ ] Final note in PR / changelog

**Notes:**

---

## Baseline metrics (2026-08-03)

Record before/after when starting each phase:

| Metric | Baseline | Current |
|--------|----------|---------|
| `ShellHost::Instance` call sites (src, non-test) | ~290 | ~290 (unchanged; settings decoupled first) |
| `ChatController::Instance` call sites | ~70 | ~70 |
| `SettingsController::Instance` in `ShellHost.cpp` | 3 | **0** |
| `ShellHost::Instance` in `SettingsController.cpp` | ~34 | **0** |
| Controllers with `Hub()` / `BindMessaging` | Shell, Chat, Contacts, Call, … | unchanged |
| ShellHost ↔ SettingsController cross-calls | yes (bidirectional) | **no** |

Refresh with:

```bash
rg -c 'ShellHost::Instance' src --glob '!**/tests/**' | awk -F: '{s+=$2} END {print s}'
rg -c 'ChatController::Instance' src --glob '!**/tests/**' | awk -F: '{s+=$2} END {print s}'
```

---

## Risk register

| Risk | Mitigation |
|------|------------|
| Large Phase 8 churn | Defer until facades + ports exist; migrate one presenter at a time |
| RmlUi static callbacks need global pointer | App stores presenter ptr in registrar closure; document lifetime |
| Behavior regressions in nav/dismiss order | Keep ShellInterruption tests; manual Escape-order checklist |
| Thread bugs on async actions | Reuse `ProfileUnlockPorts::run_heavy` pattern; UI replies via `BrowserThread::PostTask(UI, …)` |
