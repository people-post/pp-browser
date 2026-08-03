# UI ↔ functional migration plan

> **Temporary working doc.** Track phased decoupling of UI presenters from functional singletons.  
> **Delete this file** when all phases below are complete and verified.  
> **Architecture reference (permanent):** [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md)

**Last updated:** 2026-08-03  
**Status:** Phase 5 complete; Phase 6 partial (chat off Hub via MessagingChatPorts; call/shell remain)

---

## How to use this file

1. Work phases **in order** unless a later phase has zero deps on incomplete earlier work.
2. Mark items `[x]` when done; add PR links or commit SHAs in *Notes* if helpful.
3. On resume: search repo for `ShellHost::Instance`, `ChatController::Instance`, and controller `Hub()` — counts should trend down.
4. Do **not** add new `::Instance()` call sites or new controller → controller singleton calls.

---

## Success criteria (global)

- [ ] No functional module writes `ShellHost::State()` or calls another presenter `::Instance()`.
- [x] Settings, contacts, people-picker, client-compat, and **chat** have no `BindMessaging` / `Hub()` on controllers.
- [ ] Chat and shell reach messaging only via facades or app-wired ports. — **chat done**; shell/call still bind hub directly
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
- [x] `ChatController` + sub-presenters use shell ports for nav/feedback; Setup bootstrap uses `ShellSetupPorts`
- [x] Define `ShellFeedbackPorts` + `ShellFeedbackChromePorts` — toast, banner, dismiss, confirm, prompt
- [x] Wire feedback + navigation ports in `Application.cpp`; clear via `Bind*({})` on shutdown
- [x] Settings / contacts / chat confirm dialogs use `ShellFeedbackPorts`
- [x] `PeoplePickerController` shell nav + feedback ports (layers via extended `ShellNavigationPorts`)
- [ ] RmlUi callbacks → presenter method → port (no `Hub()` in callback path) — hub still used for messaging ops on chat/call/picker

**Exit check:** Feedback helpers do not call `ShellHost::Instance()` — **done**. Contacts shell decoupling — **done**. Chat shell/feedback + Setup bootstrap — **done** (`ShellSetupPorts`).

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
- [x] Remaining: ~~`FlowCoordinator`, `DeferredStartup`, `ClientCompatController`~~ — done via shell ports
- [x] ChatController Setup bootstrap — `ShellSetupPorts` (Initialize, fonts_ready, RegisterPane, Update, SyncLayout)
- [ ] Document remaining **UI-owned** fields in `ShellState` vs presenter-private state

**Exit check:** Grep `ShellHost::Instance()` from `feature/chat/ChatController.cpp` — **0**. All feature presenters decoupled from shell singleton.

**Notes:** `ShellPinGatePorts`, `ShellCallChromePorts`; extended `ShellNavigationPorts` with badges, auxiliary, layers, `fonts_ready`. `FlowCoordinator` uses `close_layer` port; `DeferredStartup` receives navigation ports from Application.

---

## Phase 6 — Messaging facade (narrow hub exposure)

**Goal:** UI never includes `MessagingHub.h` for orchestration; only facade + views.

**Primary files:** new `MessagingFacade.*` (likely `feature/messaging/`), `ChatController.*`, `ShellHost.*`, `ContactsController.*`, `Application.cpp`

- [x] Introduce `MessagingContactsPorts` + `MakeMessagingContactsPorts` (contacts State + Actions)
- [x] Introduce `MessagingPeoplePickerPorts` + `MessagingCompatPorts` (narrow hub slices)
- [x] Introduce `MessagingChatPorts` + `MakeMessagingChatPorts` for chat / inbox / P2P / groups / router
- [x] Introduce `ShellSetupPorts` for chat Setup bootstrap (pane registration, initial layout)
- [ ] Introduce full `MessagingFacade` umbrella (optional consolidation of port structs)
- [ ] `Application` owns hub; facade holds reference or pointer to hub internals — **partial** (ports wired in Application)
- [x] Remove `BindMessaging` / `Hub()` from **contacts** controller
- [x] Remove `BindMessaging` / `Hub()` from **people-picker** controller
- [x] Remove `BindMessaging` / `Hub()` from **client-compat** controller
- [x] Remove `BindMessaging` / `Hub()` from **chat** controller (+ `ChatThreadChrome`, `ChatTranscriptScroller`)
- [ ] Remove `BindMessaging` / `Hub()` from shell, call
- [ ] Migrate call sites incrementally (call next, then shell reachability badges)

**Exit check:** No `MessagingHub& Hub()` on **ContactsController**, **PeoplePickerController**, **ClientCompatController**, **ChatController** — **done**.

**Notes:** `MessagingContactsPorts.h`, `MessagingPeoplePickerPorts.h`, `MessagingCompatPorts.h`, `MessagingChatPorts.h`, `ShellSetupPorts.h` in feature layer; `RegisterMessagingTools` wired from `Application.cpp` (composition root).

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
| `ShellHost::Instance` call sites (src, non-test) | ~290 | **~32** (app root + ShellHost internals) |
| `ChatController::Instance` call sites | ~70 | ~70 |
| `SettingsController::Instance` in `ShellHost.cpp` | 3 | **0** |
| `ShellHost::Instance` in `SettingsController.cpp` | ~34 | **0** |
| `ShellHost::Instance` in `ContactsController.cpp` | ~30 | **0** |
| `ShellHost::Instance` in `ChatController.cpp` | ~99 | **0** |
| `ShellHost::Instance` in `ChatController.cpp` (Setup bootstrap) | ~10 | **0** (`ShellSetupPorts`) |
| `ShellHost::Instance` in `PinGateController.cpp` | ~15 | **0** |
| `ShellHost::Instance` in `CallController.cpp` | ~12 | **0** |
| `ShellHost::Instance` in `BadgeAggregator.cpp` | 1 | **0** |
| `ShellHost::Instance` in `WorkingSetController.cpp` | 6 | **0** |
| `ShellHost::Instance` in `PeoplePickerController.cpp` | ~20 | **0** |
| `ShellHost::Instance` in `FlowCoordinator.cpp` | 1 | **0** |
| `ShellHost::Instance` in `DeferredStartup.cpp` | 4 | **0** |
| `ShellHost::Instance` in `ClientCompatController.cpp` | 2 | **0** |
| Controllers with `Hub()` / `BindMessaging` | Shell, Chat, Contacts, Call, … | Contacts, PeoplePicker, ClientCompat, **Chat removed**; Call/Shell remain |
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
