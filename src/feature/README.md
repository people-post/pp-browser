# `src/feature`

Product **orchestration** for pp-browser: hubs, sessions, and workflows that **wire** domain peers (and foundation). Presenters / shell live under `feature/ui/**` **today** as staging for top-level **`src/gui/`** ([F008](../../projects/feature-layer-reorg/DECISIONS.md#f008--gui-layer-above-feature)).

```
app → gui → feature → domain → foundation → common → pp_common   # target (f7)
app → feature → …                                              # today (ui still under feature)
feature    ← you are here
```

Includes use `foundation/…` and `domain/…` (see [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md)). Ongoing cleanup: [`projects/feature-layer-reorg/`](../../projects/feature-layer-reorg/) (sure peels first; working North Star).

**Rule:** dependencies flow downward only. Feature may use `foundation/`, `domain/`, `common/`, and `lib/`; it must not `#include` from `app/` (and must not `#include` from `gui/` once that layer ships). Runtime module wiring: [`docs/architecture/RUNTIME_COMPOSITION.md`](../../docs/architecture/RUNTIME_COMPOSITION.md).

Each top-level folder (and `ai/tools`, `ai/bindings`, plus bands `messaging/calls/`, `ui/{shell,contacts,chat}/`) builds into **`pp_feature_<module>`** libraries. The aggregate **`pp_feature`** (`INTERFACE`) links them for app code. Top-level `feature/chat` is **retired** ([F007](../../projects/feature-layer-reorg/DECISIONS.md#f007--vocabulary--end-state-feature-names)). End-state product UI folder is **`gui`**, not another `ui` next to `domain/ui`.

---

## What belongs here

Feature code should **coordinate** multiple base modules into a screen, session, or workflow.

| Put it in feature when… | Put it in domain / foundation when… |
|-------------------------|-------------------------------------|
| It coordinates several domain modules into a user flow | It owns a data model, store, client, or codec |
| It **binds** `common` contracts to concrete types | It talks to one external system (HTTP, SQLite, libsodium) |
| It implements a screen, controller, or session lifecycle | It is reusable across multiple features without UI |
| It is only meaningful in one UI context | Foundation: shared kernel every peer may link |

If you are unsure, ask: *"Could another feature import this without pulling in a specific screen?"* No → feature. Yes → domain (or foundation if every peer needs the impl).

---

## Module map

Four top-level folders (+ bands). Two sub-trees under `ai/`.

```
src/feature/
├── settings/     Config apply logic, section handlers + SettingsTools
├── ai/           Agent session, turn pipeline, UI generation
│   ├── tools/
│   └── bindings/
├── messaging/    Conversations hub (legacy name) + delivery; MessagingFacade
│   └── calls/    Call session band (f4v1 → future feature/calls)
└── ui/           Presenters + shell/contacts/chat (pp_feature_ui) — **staging for src/gui/** (F008)
    ├── shell/    ShellHost, mount, gestures, shell ports
    ├── contacts/ Contacts + people-picker
    └── chat/     ChatController + screen helpers (absorbed from feature/chat)
```

`IToolProvider` / `ToolRegistry` live in `domain/ai/` so settings and messaging can register tools without linking `pp_feature_ai`.

**Domain grouping (mental model):**

| Domain | Modules | Typical question it answers |
|--------|---------|----------------------------|
| **Configuration** | settings | How do user edits merge into `AppConfig`? |
| **Intelligence** | ai, ai/tools, ai/bindings | How does the agent plan turns, call tools, and bind RmlUi actions? |
| **Conversations** | messaging (+ calls band) | How do threads sync/relay; how do call sessions run? |
| **Presentation (staging)** | `feature/ui` bands | Shell/contacts/chat presenters → lift to **`gui/`** (not `domain/ui`) |

Start points when exploring:

- Agent session → `ai/AgentSession.h`, `ai/TurnPlanner.h`, `ai/TurnExecutor.h`
- Conversations hub → `messaging/MessagingHub.h`, `messaging/MeshMessagingService.h`
- Call session → `messaging/calls/CallStack.h`
- Window shell → `ui/shell/ShellHost.h`, `ui/SettingsController.h`
- Chat screen → `ui/chat/ChatController.h`, `ui/chat/MessagingTools.h`
- Settings apply → `settings/SettingsLogic.h`, `settings/SettingsSectionHandler.h`

Includes use the repo root: `#include "feature/ui/chat/ChatController.h"`.

---

## Dependency design

**Goal:** acyclic feature modules that link only lower layers, arranged so shared orchestration types live in the module that owns the workflow.

### Cross-layer direction

```
app → feature → domain → foundation → common
```

Feature modules always link `pp_base` and `pp_common` (via `pp_browser_add_feature_library` in [`cmake/PpBrowserFeature.cmake`](../../cmake/PpBrowserFeature.cmake)). Production code has no upward `#include` edges: `foundation/`/`domain/` do not include `feature/`, and `feature/` does not include `app/`. Domain peers must not gain new edges to each other — peel those via `common` ports and feature wiring ([SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md)).

### Intra-feature direction

Intended link order (each `PUBLIC_LIBS` only lower feature modules):

```
settings, ai/tools, ai/bindings
  ↑
ai
  ↑
messaging
  ↑
ui
```

**Principles for new code:**

1. **Downward includes only** — when module A needs a type from B, B should not include A's headers. Within feature: `settings` must not include `messaging/` or `ui/`; `messaging` must not include `ui/`.
2. **Shared structs go low** — if feature and domain both need a DTO, move it to the owning domain module (or a dedicated `*Types.h` there / `common` if two peers need it).
3. **Include legal deps; fwd-decl to break cycles** — if a type is already a legal dependency (lower layer or allowed feature edge), `#include` its header in the `.h` that names it. Do not forward-declare `foundation/`/`domain/`/`common/` types just to keep headers lean. Forward declarations are for cycle-breaking and forbidden upward edges. Prefer small ports/`*Types.h` headers when that avoids pulling an unrelated heavy tree — repo rule: [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md#prefer-include-over-forward-declaration).
4. **Cross-controller wiring stays in app** — tool registration, tab ticks, and `ActionRouter` model-dirty callbacks belong in `src/app/`, not feature headers.
5. **Fork glue stays at the edge** — RmlUi via `pp_foundation_platform` in `ui/` and `ai/bindings/`; Amp/mesh via `pp_domain_mesh` in `messaging/` (forks under `src/lib/` / FetchContent).

Feature/app cleanup tracking: [`projects/feature-layer-reorg/`](../../projects/feature-layer-reorg/).

---

## Current state

The dependency hierarchy above is **enforced at the header level** for upward feature-module edges. Regressions are caught by [`scripts/check_feature_includes.sh`](../../scripts/check_feature_includes.sh).

### Cycle-breaking patterns

| Pattern | Location | Purpose |
|---------|----------|---------|
| `ChatSessionPorts` | `ui/ChatSessionPorts.h` | Injected chat nav ports for contacts/people-picker; Application fills from `ChatController` |
| `CallActionsPorts` | `ui/CallActionsPorts.h` | Call chrome/actions for chat, shell, people-picker; Application fills from `CallController` |
| `CallFunctionalPorts` | `messaging/calls/CallFunctionalPorts.h` | Functional call ports for `CallController`; Application fills via `MakeCallFunctionalPorts` + owned `CallUiBackend` |
| `CallUiBackend` | `messaging/calls/CallUiBackend.h` | Sealed façade over `CallStack` session/lifecycle |
| `CallStack` | `messaging/calls/CallStack.h` | Owns call media/CSM/lifecycle/bridge; Hub holds `unique_ptr<CallStack>` |
| `ContactsNotifyPorts` | `ui/contacts/ContactsNotifyPorts.h` | Contacts refresh/select for chat; Application fills from `ContactsController` |
| `UnlockEnsurePorts` | `ui/UnlockEnsurePorts.h` | Ensure unlocked / unlock-in-progress; Application fills from `ProfileUnlockGate` |
| `FlowCoordinatorPorts` | `ui/FlowCoordinatorPorts.h` | Modal begin/end/dismiss; Application fills from `FlowCoordinator` |
| `BadgeNotifyPorts` | `ui/BadgeNotifyPorts.h` | Badge refresh / sessions unread for chat; Application fills from `BadgeAggregator` |
| `PinGateActionPorts` | `ui/PinGateActionPorts.h` | PIN overlay submit/cancel/chooser; Application fills from `PinGateController` |
| `PeoplePickerNotifyPorts` | `ui/contacts/PeoplePickerNotifyPorts.h` | Open-picker hooks for chat/call; Application fills from `PeoplePickerController` |
| `SettingsCommands` | `settings/SettingsCommands.h` | All settings cross-module ports (member on `SettingsController`); Application binds — no messaging bind |
| `ShellNavigationPorts` | `ui/shell/ShellNavigationPorts.h` | Shell layout/nav for settings, chat, contacts; app fills via `MakeShellNavigationPorts` |
| `ShellFeedbackPorts` | `ui/shell/ShellFeedbackPorts.h` | Toast/banner/dialog; app fills via `BindSharedShellFeedback` |
| `MessagingUiPorts` | `messaging/MessagingUiPorts.h` | Read-only `MessagingView` for chat presenter |
| `MessagingFacade` | `messaging/MessagingFacade.h` | Non-owning wrapper over `MessagingHub&`; chat / chat sub-presenters / messaging tools / settings+badge wiring call its methods (replaces the `MessagingChatPorts` mega-struct) |
| `AgentUiPorts` | `messaging/AgentUiPorts.h` | Agent facade for chat; Application owns `AgentSession` |
| UI ↔ functional boundary | [`docs/architecture/UI_FUNCTIONAL_BOUNDARY.md`](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md) | State / Config / Actions / Events; app-owned presenters + ports |
| App-owned presenters | `app/Application.cpp` | `unique_ptr` for shell + all presenters; `InstallInstance` for RmlUi static callbacks |
| `ProfileIdentityView` | `domain/people/ProfileIdentityView.h` | Shared identity presentation DTO (filled by `MessagingHub`) |
| SessionStore listeners + nested service slices | `SessionStore`, nested `*::Apply` types, `ConfigApplyBridge` | Settings flush persists disk DTOs; app projects slices so settings UI does not own service apply |
| Hub-and-spoke within messaging | `MessagingHub` (`MessagingCore`) owns stores/inbox/P2P + `MeshHost` + `CallStack` | Assembler inside `pp_feature_messaging`; mesh shared with `pp-node` via MeshHost |
| App-level wiring | `app/Application.cpp`, `app/ConfigApplyBridge.cpp` | Cross-controller callbacks and SessionStore → slice fan-out stay in `app/` per SRC_LAYOUT |

### Intentional one-way edges (not cycles)

| From | To | Why |
|------|-----|-----|
| `messaging/` | `feature/ai/AgentSession.h` | Route inbound messages to the agent session |
| `ui/` | `feature/settings/*`, `feature/messaging/*` | Settings/shell/chat presenters need hub + agent wiring |

### Test-layer dependencies

| Test target | Links | Notes |
|-------------|-------|-------|
| `pp_browser_*_test` (base) | `pp_common` + module under test | Base macro in [`cmake/PpBrowserBase.cmake`](../../cmake/PpBrowserBase.cmake) |
| `pp_browser_feature_*_test` | `pp_feature` + `pp_base` + `pp_common` | By design — feature tests may pull the full stack |
| `rmlui_unit_tests` | doctest + RmlUi fork | Built/run in **pp-cpp-ui** (`PP_UI_BUILD_TESTS`); includes fork `ClickRouting` cases |

Place tests at the **highest layer they include or link** (see SRC_LAYOUT). Base tests must not depend on `pp_feature`.

**Where active development lives**

| Area | Feature role | Base / project pointer |
|------|--------------|------------------------|
| Agent turns | `AgentSession`, turn pipeline | `domain/ai/` (LlmClient, TurnPlan, conversation) |
| P2P messaging | `MessagingHub`, sync, relay | [`docs/architecture/P2P_MESSAGING.md`](../../docs/architecture/P2P_MESSAGING.md) |
| Window shell | `ShellHost`, document loading | [`docs/ui/WINDOW_SHELL.md`](../../docs/ui/WINDOW_SHELL.md) |
| Chat UI | `ChatController`, messaging tools | `assets/views/chat.rml`, `domain/messaging/` stores |
| Settings | Section handlers, config merge | `assets/views/settings.rml`, `foundation/data/Config.h` |
| At-rest PIN gate | `ProfileUnlockGate` + `PinGateController` UI | [`projects/at-rest-crypto/`](../../projects/at-rest-crypto/) |
| Feature/app reorg | Peels + module splits; end-state **conversations** / **calls** (no top-level **chat/**) | [`projects/feature-layer-reorg/`](../../projects/feature-layer-reorg/) ([F007](../../projects/feature-layer-reorg/DECISIONS.md#f007--vocabulary--end-state-feature-names)) |

---

## Adding or changing code

1. Find the module that **owns** the screen, session, or multi-module workflow.
2. Follow the dependency principles above; respect CMake `PUBLIC_LIBS` order in the owning folder's `CMakeLists.txt`.
3. Add tests in `src/feature/<module>/tests/` (`*_test.cpp` files). One executable per folder is created automatically (`pp_browser_feature_<module>_test`).
4. Run [`scripts/check_feature_includes.sh`](../../scripts/check_feature_includes.sh) and [`scripts/check_platform_ifdefs.sh`](../../scripts/check_platform_ifdefs.sh) before pushing.
5. Document externally visible behavior in [`docs/contracts/`](../../docs/contracts/) or [`docs/ui/`](../../docs/ui/) when wire formats or UI contracts change.

---

## Further reading

| Doc | Why |
|-----|-----|
| [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) | Five-layer layout (common/foundation/domain/feature/app), test placement |
| [`projects/feature-layer-reorg/`](../../projects/feature-layer-reorg/) | Feature/app cleanup: sure peels, working North Star, phases |
| [`docs/architecture/UI_FUNCTIONAL_BOUNDARY.md`](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md) | UI vs functional systems; state, config, actions, events; app-owned presenters |
| [`docs/architecture/ARCHITECTURE.md`](../../docs/architecture/ARCHITECTURE.md) | System overview (SDL, RmlUi, agent, shell) |
| [`docs/architecture/P2P_MESSAGING.md`](../../docs/architecture/P2P_MESSAGING.md) | Messaging hub and P2P orchestration |
| [`docs/ui/WINDOW_SHELL.md`](../../docs/ui/WINDOW_SHELL.md) | Shell layout and document hosting |
| [`src/foundation/README.md`](../foundation/README.md) / [`src/domain/README.md`](../domain/README.md) | Foundation + domain primitives and dependency rules |
| [`AGENTS.md`](../../AGENTS.md) | Agent-oriented map of the whole repo |
