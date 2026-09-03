# `src/feature`

Product **orchestration** for pp-browser: screens, controllers, sessions, and hubs that **wire** domain peers (and foundation) into user-facing workflows.

```
app → feature → domain → foundation → common → pp_common
feature    ← you are here
```

Includes use `foundation/…` and `domain/…` (see [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md)). Ongoing feature/app cleanup: [`projects/feature-layer-reorg/`](../../projects/feature-layer-reorg/) (sure peels first; working North Star).

**Rule:** dependencies flow downward only. Feature may use `foundation/`, `domain/`, `common/`, and `lib/`; it must not `#include` from `app/`. Runtime module wiring: [`docs/architecture/RUNTIME_COMPOSITION.md`](../../docs/architecture/RUNTIME_COMPOSITION.md).

Each top-level folder (and `ai/tools`, `ai/bindings`) builds as its own static library — **`pp_feature_<module>`** (e.g. `pp_feature_messaging`, `pp_feature_chat`). The aggregate **`pp_feature`** (`INTERFACE`) links all module libraries for app code. See [`CMakeLists.txt`](CMakeLists.txt) and per-folder `CMakeLists.txt` files.

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

Five top-level folders. Two sub-trees under `ai/`.

```
src/feature/
├── settings/     Config apply logic, section handlers + SettingsTools (no messaging/chat/ai deps)
├── ai/           Agent session, turn pipeline, UI generation; BuildToolRegistryFromConfig
│   ├── tools/        Web search, MCP tool adapters
│   └── bindings/     RmlUi action routing, bindings manifest
├── messaging/    MessagingHub (MessagingCore assembler), MeshHost consumer, CallStack, MessagingFacade
├── ui/           Shell, settings/contacts controllers, RML mount, ChatSessionPorts
└── chat/         Chat controller, agent + MessagingFacade wiring, messaging agent tools
```

`IToolProvider` / `ToolRegistry` live in `domain/ai/` so settings and messaging can register tools without linking `pp_feature_ai`.

**Domain grouping (mental model):**

| Domain | Modules | Typical question it answers |
|--------|---------|----------------------------|
| **Configuration** | settings | How do user edits merge into `AppConfig`? |
| **Intelligence** | ai, ai/tools, ai/bindings | How does the agent plan turns, call tools, and bind RmlUi actions? |
| **Connectivity** | messaging | How do threads sync, relay, and route through P2P? |
| **Presentation** | ui | How does the shell host documents, settings, and contacts? |
| **Chat surface** | chat | How does the chat screen wire agent, hub, and shell together? |

Start points when exploring:

- Agent session → `ai/AgentSession.h`, `ai/TurnPlanner.h`, `ai/TurnExecutor.h`
- Messaging hub → `messaging/MessagingHub.h`, `messaging/MeshMessagingService.h`
- Window shell → `ui/ShellHost.h`, `ui/SettingsController.h`
- Chat screen → `chat/ChatController.h`, `chat/MessagingTools.h`
- Settings apply → `settings/SettingsLogic.h`, `settings/SettingsSectionHandler.h`

Includes use the repo root: `#include "feature/chat/ChatController.h"`.

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
  ↑
chat
```

**Principles for new code:**

1. **Downward includes only** — when module A needs a type from B, B should not include A's headers. Within feature: `settings` must not include `messaging/`, `ui/`, or `chat/`; `messaging` must not include `ui/` or `chat/`.
2. **Shared structs go low** — if feature and base both need a DTO, move it to the owning base module (or a dedicated `*Types.h` there).
3. **Include legal deps; fwd-decl to break cycles** — if a type is already a legal dependency (lower layer or allowed feature edge), `#include` its header in the `.h` that names it. Do not forward-declare `base/`/`common/` types just to keep headers lean. Forward declarations are for cycle-breaking and forbidden upward edges. Prefer small ports/`*Types.h` headers when that avoids pulling an unrelated heavy tree — repo rule: [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md#prefer-include-over-forward-declaration).
4. **Cross-controller wiring stays in app** — tool registration, tab ticks, and `ActionRouter` model-dirty callbacks belong in `src/app/`, not feature headers.
5. **Fork glue stays at the edge** — RmlUi via `pp_foundation_platform` in `ui/`, `chat/`, and `ai/bindings/`; libp2p via `pp_domain_mesh` in `messaging/` (forks under `src/lib/`).

---

## Current state

The dependency hierarchy above is **enforced at the header level** for upward feature-module edges. Regressions are caught by [`scripts/check_feature_includes.sh`](../../scripts/check_feature_includes.sh).

### Cycle-breaking patterns

| Pattern | Location | Purpose |
|---------|----------|---------|
| `ChatSessionPorts` | `ui/ChatSessionPorts.h` | Injected chat nav ports for contacts/people-picker; Application fills from `ChatController` |
| `CallActionsPorts` | `ui/CallActionsPorts.h` | Call chrome/actions for chat, shell, people-picker; Application fills from `CallController` |
| `CallFunctionalPorts` | `messaging/CallFunctionalPorts.h` | Functional call ports for `CallController`; Application fills via `MakeCallFunctionalPorts` + owned `CallUiBackend` |
| `CallUiBackend` | `messaging/CallUiBackend.h` | Sealed façade over `CallStack` session/lifecycle (bound to `MessagingHub::CallStackRef()`; no leaky CSM/Lifecycle ports) |
| `CallStack` | `messaging/CallStack.h` | Owns call media/CSM/lifecycle/bridge/CallMediaDirect/relay+dial+circuit clients; Hub holds `unique_ptr<CallStack>` and forwards `Calls()`/`Lifecycle()` |
| `ContactsNotifyPorts` | `ui/ContactsNotifyPorts.h` | Contacts refresh/select for chat; Application fills from `ContactsController` |
| `UnlockEnsurePorts` | `ui/UnlockEnsurePorts.h` | Ensure unlocked / unlock-in-progress; Application fills from `ProfileUnlockGate` |
| `FlowCoordinatorPorts` | `ui/FlowCoordinatorPorts.h` | Modal begin/end/dismiss; Application fills from `FlowCoordinator` |
| `BadgeNotifyPorts` | `ui/BadgeNotifyPorts.h` | Badge refresh / sessions unread for chat; Application fills from `BadgeAggregator` |
| `PinGateActionPorts` | `ui/PinGateActionPorts.h` | PIN overlay submit/cancel/chooser; Application fills from `PinGateController` |
| `PeoplePickerNotifyPorts` | `ui/PeoplePickerNotifyPorts.h` | Open-picker hooks for chat/call; Application fills from `PeoplePickerController` |
| `SettingsCommands` | `settings/SettingsCommands.h` | All settings cross-module ports (member on `SettingsController`); Application binds — no messaging bind |
| `ShellNavigationPorts` | `ui/ShellNavigationPorts.h` | Shell layout/nav for settings, chat, contacts; app fills via `MakeShellNavigationPorts` |
| `ShellFeedbackPorts` | `ui/ShellFeedbackPorts.h` | Toast/banner/dialog; app fills via `BindSharedShellFeedback` |
| `MessagingUiPorts` | `messaging/MessagingUiPorts.h` | Read-only `MessagingView` for chat presenter |
| `MessagingFacade` | `messaging/MessagingFacade.h` | Non-owning wrapper over `MessagingHub&`; chat / chat sub-presenters / messaging tools / settings+badge wiring call its methods (replaces the `MessagingChatPorts` mega-struct) |
| `AgentUiPorts` | `messaging/AgentUiPorts.h` | Agent facade for chat; Application owns `AgentSession` |
| UI ↔ functional boundary | [`docs/architecture/UI_FUNCTIONAL_BOUNDARY.md`](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md) | State / Config / Actions / Events; app-owned presenters + ports |
| App-owned presenters | `app/Application.cpp` | `unique_ptr` for shell + all presenters; `InstallInstance` for RmlUi static callbacks |
| `ProfileIdentityView` | `base/people/ProfileIdentityView.h` | Shared identity presentation DTO (filled by `MessagingHub`) |
| SessionStore listeners + nested service slices | `SessionStore`, nested `*::Apply` types, `ConfigApplyBridge` | Settings flush persists disk DTOs; app projects slices so settings UI does not own service apply |
| Hub-and-spoke within messaging | `MessagingHub` (`MessagingCore`) owns stores/inbox/P2P + `MeshHost` + `CallStack` | Assembler inside `pp_feature_messaging`; mesh shared with `pp-node` via MeshHost |
| App-level wiring | `app/Application.cpp`, `app/ConfigApplyBridge.cpp` | Cross-controller callbacks and SessionStore → slice fan-out stay in `app/` per SRC_LAYOUT |

### Intentional one-way edges (not cycles)

| From | To | Why |
|------|-----|-----|
| `messaging/` | `feature/ai/AgentSession.h` | Route inbound messages to the agent session |
| `ui/` | `feature/settings/*`, `feature/messaging/MessagingHub.h` | Settings sections and shell need hub access |
| `chat/` | `feature/ai/*`, `feature/messaging/*`, `feature/ui/*` | Top-layer screen composes all lower feature modules |

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
| Settings | Section handlers, config merge | `assets/views/settings.rml`, `base/data/Config.h` |
| At-rest PIN gate | `ProfileUnlockGate` + `PinGateController` UI | [`projects/at-rest-crypto/`](../../projects/at-rest-crypto/) |

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
| [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) | Five-layer layout (common/lib/base/feature/app), test placement |
| [`docs/architecture/UI_FUNCTIONAL_BOUNDARY.md`](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md) | UI vs functional systems; state, config, actions, events; app-owned presenters |
| [`docs/architecture/ARCHITECTURE.md`](../../docs/architecture/ARCHITECTURE.md) | System overview (SDL, RmlUi, agent, shell) |
| [`docs/architecture/P2P_MESSAGING.md`](../../docs/architecture/P2P_MESSAGING.md) | Messaging hub and P2P orchestration |
| [`docs/ui/WINDOW_SHELL.md`](../../docs/ui/WINDOW_SHELL.md) | Shell layout and document hosting |
| [`src/foundation/README.md`](../foundation/README.md) / [`src/domain/README.md`](../domain/README.md) | Foundation + domain primitives and dependency rules |
| [`AGENTS.md`](../../AGENTS.md) | Agent-oriented map of the whole repo |
