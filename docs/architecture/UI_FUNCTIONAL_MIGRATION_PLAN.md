# UI ↔ functional migration plan

> **Temporary working doc.** Track phased decoupling of UI presenters from functional singletons.  
> **Delete this file** when all phases below are complete and verified.  
> **Architecture reference (permanent):** [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md)

**Last updated:** 2026-08-03  
**Status:** Phases 1–5 largely done; Phase 6 partial (contacts off Hub); Phase 7 partial (AgentUiPorts header)

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

**Notes:** `MakeShellNavigationPorts`, `BindSharedShellFeedback` in `src/feature/ui/`; shell lifecycle wiring in `Application.cpp` (`WireShellPresentationEvents`).

---

## Phase 2 — Extract Actions ports (chat, contacts, shell feedback)

**Goal:** Imperative UI operations go through ports; no new hub pointers on presenters.

**Primary files:** `ChatController.*`, `ContactsController.*`, `PeoplePickerController.*`, `UserFeedback.cpp`, `ShellFeedback.cpp`, `Application.cpp`

- [ ] Define `ChatActions` (hub imperative ops without `Hub()` on presenter surface) — deferred to Phase 6 facade
- [x] `ContactsController` uses `ShellNavigationPorts` + `ShellFeedbackPorts` (zero `ShellHost::Instance()`)
- [x] `ChatController` + `ChatThreadChrome` use shell ports for nav/feedback (Setup bootstrap only still touches `ShellHost`)
- [x] Define `ShellFeedbackPorts` + `ShellFeedbackChromePorts` — toast, banner, dismiss, confirm, prompt
- [x] Wire feedback + navigation ports in `Application.cpp`; clear via `Bind*({})` on shutdown
- [x] Settings / contacts / chat confirm dialogs use `ShellFeedbackPorts`
- [x] `PeoplePickerController` shell nav + feedback ports (layers via extended `ShellNavigationPorts`)
- [ ] RmlUi callbacks → presenter method → port (no `Hub()` in callback path) — hub still used for messaging ops on chat/call/picker

**Exit check:** Feedback helpers do not call `ShellHost::Instance()` — **done**. Contacts shell decoupling — **done**. Chat shell/feedback — **done** (except Setup bootstrap).

---

## Phase 3 — Extract read-only State views

**Goal:** Presenters bind from snapshots, not live mutable service internals.

**Primary files:** `MessagingHub.*`, `ChatController.*`, `ShellHost.*`, `BadgeAggregator.*`, new `*View.h` in `base/` or feature as appropriate

- [x] `MessagingView` + `MessagingUiPorts` — initialized, ready, has_router, active_thread_id (`ProjectMessagingView`)
- [x] Extended `ShellChromeSnapshot` — nav badges, primary pane, contact/settings detail flags, banner_message, auxiliary_open
- [ ] `ChatView` — thread list summary, composer state (UI-owned draft stays in presenter)
- [ ] Presenter `Tick()`: full `Snapshot()` → diff → `DataModelHost::Dirty` pattern
- [ ] Stop exposing raw mutable hub state to RmlUi bind paths

**Exit check:** Chat tick does not read messaging internals except through `MessagingView` / facade — **partial** (ports bound; hub still primary).

**Notes:** Chat `compose_disabled` can use `MessagingUiPorts` snapshot at Setup.

---

## Phase 4 — Events channel

**Goal:** Functional-initiated UI updates use push, not ad-hoc polling and hidden flags.

**Primary files:** `Application.cpp`, `MessagingHub.*`, presenters, `BadgeAggregator.*`

- [x] Shell lifecycle events moved to `Application::WireShellPresentationEvents` (nav tab, layout mode/sync, transient pop, account sheet)
- [ ] List remaining one-shot events: inbound message, call state, unlock dismissed
- [ ] Add small listener interfaces or app-wired hooks on facades
- [x] Hub `SetOnMessagingReady` / reachability remain in `Application` (not ChatController::Setup)
- [ ] `BadgeAggregator` event-driven refresh (still poll-on-nav-tab; writes via port)

**Exit check:** No `SetOnNavTabChanged` / layout hooks in `ChatController::Setup` — **done**.

**Notes:**

---

## Phase 5 — Stop functional → shell state writes

**Goal:** Vault, call, and chat logic do not mutate `ShellState` directly.

**Primary files:** `PinGateController.*`, `CallController.*`, `ChatController.*`, `ProfileUnlockGate.*`, `ShellHost.*`

- [x] PIN gate: `ShellPinGatePorts` — no direct `ShellHost::State().pin_gate` from `PinGateController`
- [x] Call chrome: `ShellCallChromePorts` — `CallController` mutates ring/in-call via ports
- [x] Chat navigation: `ShellNavigationPorts` instead of `ShellHost::Instance()` for pane/tab/compact chat/activity
- [x] `BadgeAggregator` → `set_nav_badges` port
- [x] `WorkingSetController` auxiliary pane via `ShellNavigationPorts`
- [x] `PeoplePickerController` layers/nav/toast via shell ports
- [ ] Remaining: `FlowCoordinator`, `DeferredStartup`, `ClientCompatController` (dialog/fonts)
- [ ] Document remaining **UI-owned** fields in `ShellState` vs presenter-private state

**Exit check:** Grep `ShellHost::Instance()` from `feature/chat/ChatController.cpp` — **Setup bootstrap only** (~10 lines: Initialize, RegisterPane, SyncLayout, fonts_ready).

**Notes:** `ShellPinGatePorts`, `ShellCallChromePorts`; extended `ShellNavigationPorts` with badges, auxiliary, layers.

---

## Phase 6 — Messaging facade (narrow hub exposure)

**Goal:** UI never includes `MessagingHub.h` for orchestration; only facade + views.

**Primary files:** new `MessagingFacade.*` (likely `feature/messaging/`), `ChatController.*`, `ShellHost.*`, `ContactsController.*`, `Application.cpp`

- [x] Introduce `MessagingContactsPorts` + `MakeMessagingContactsPorts` (contacts State + Actions)
- [ ] Introduce full `MessagingFacade` for chat / shell / call
- [ ] `Application` owns hub; facade holds reference or pointer to hub internals
- [x] Remove `BindMessaging` / `Hub()` from **contacts** controller
- [ ] Remove `BindMessaging` / `Hub()` from chat, shell, call, people-picker
- [ ] Migrate call sites incrementally (chat first, then shell reachability badges, then picker)

**Exit check:** No `MessagingHub& Hub()` on **ContactsController** — **done**. Hub header not included from presenter headers unless unavoidable during tail migration.

**Notes:** `MessagingContactsPorts.h` in `feature/messaging/`.

---

## Phase 7 — Agent facade

**Goal:** Chat/settings agent UI uses `AgentFacade` (turn status, invoke, cancel).

**Primary files:** `AgentSession.*`, `ChatController.*`, `ActionRouter.*`, `Application.cpp`

- [x] `AgentView` + `AgentUiPorts` header (read snapshot scaffold)
- [ ] Wire `AgentUiPorts` in Application / ChatController Tick
- [ ] `AgentActions` — start turn, cancel, configure slice already via bridge
- [ ] Chat presenter uses facade; `ActionRouter` remains app-owned

**Exit check:** Chat does not call `AgentSession` methods directly except through facade.

**Notes:** `AgentUiPorts.h` added; wiring deferred.

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
| `ShellHost::Instance` call sites (src, non-test) | ~290 | ~55 (app composition + Setup bootstrap + tail) |
| `ChatController::Instance` call sites | ~70 | ~70 |
| `SettingsController::Instance` in `ShellHost.cpp` | 3 | **0** |
| `ShellHost::Instance` in `SettingsController.cpp` | ~34 | **0** |
| `ShellHost::Instance` in `ContactsController.cpp` | ~30 | **0** |
| `ShellHost::Instance` in `ChatController.cpp` (non-Setup) | ~99 | **0** |
| `ShellHost::Instance` in `ChatController.cpp` (Setup bootstrap) | — | ~10 |
| `ShellHost::Instance` in `PinGateController.cpp` | ~15 | **0** |
| `ShellHost::Instance` in `CallController.cpp` | ~12 | **0** |
| `ShellHost::Instance` in `BadgeAggregator.cpp` | 1 | **0** |
| `ShellHost::Instance` in `WorkingSetController.cpp` | 6 | **0** |
| `ShellHost::Instance` in `PeoplePickerController.cpp` | ~20 | **0** |
| Controllers with `Hub()` / `BindMessaging` | Shell, Chat, Contacts, Call, … | Contacts **removed**; Chat/Call/Picker/Shell remain |
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
