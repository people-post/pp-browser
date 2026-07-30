# `src/feature`

Product **orchestration** for pp-browser: screens, controllers, sessions, and hubs that compose base primitives into user-facing workflows.

```
app        wires startup, profiles, global services
feature    ← you are here
base       stores, clients, codecs, UI building blocks
common     logger, ResultOrError, task runner (app-agnostic)
```

**Rule:** dependencies flow downward only. Feature may use `base/` and `common/`; it must not `#include` from `app/`. Repo-wide layout: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md). Runtime module wiring: [`docs/architecture/RUNTIME_COMPOSITION.md`](../../docs/architecture/RUNTIME_COMPOSITION.md).

Each top-level folder (and `ai/tools`, `ai/bindings`) builds as its own static library — **`pp_feature_<module>`** (e.g. `pp_feature_messaging`, `pp_feature_chat`). The aggregate **`pp_feature`** (`INTERFACE`) links all module libraries for app code. See [`CMakeLists.txt`](CMakeLists.txt) and per-folder `CMakeLists.txt` files.

---

## What belongs here

Feature code should **coordinate** multiple base modules into a screen, session, or workflow.

| Put it in feature when… | Put it in base when… |
|-------------------------|----------------------|
| It coordinates several base modules into a user flow | It owns a data model or wire format |
| It implements a screen, controller, or session lifecycle | It talks to one external system (HTTP, SQLite, libsodium) |
| It is only meaningful in one UI context | It is reusable across multiple features |

If you are unsure, ask: *"Could another feature import this without pulling in a specific screen?"* No → feature. Yes → base.

---

## Module map

Five top-level folders. Two sub-trees under `ai/`.

```
src/feature/
├── settings/     Config apply logic, section handlers (no messaging/chat deps)
├── ai/           Agent session, turn pipeline, UI generation
│   ├── tools/        Web search, MCP tool adapters
│   └── bindings/     RmlUi action routing, bindings manifest
├── messaging/    MessagingHub, P2P/relay/sync orchestration
├── ui/           Shell, settings/contacts controllers, RML mount, ChatSessionPorts
└── chat/         Chat controller, agent↔hub wiring, messaging agent tools
```

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
- Messaging hub → `messaging/MessagingHub.h`, `messaging/P2pMessagingService.h`
- Window shell → `ui/ShellHost.h`, `ui/SettingsController.h`
- Chat screen → `chat/ChatController.h`, `chat/MessagingTools.h`
- Settings apply → `settings/SettingsLogic.h`, `settings/SettingsSectionHandler.h`

Includes use the repo root: `#include "feature/chat/ChatController.h"`.

---

## Dependency design

**Goal:** acyclic feature modules that link only lower layers, arranged so shared orchestration types live in the module that owns the workflow.

### Cross-layer direction

```
app → feature → base → common
```

Feature modules always link `pp_base` and `pp_common` (via `pp_browser_add_feature_library` in [`cmake/PpBrowserFeature.cmake`](../../cmake/PpBrowserFeature.cmake)). Production code has no upward `#include` edges: `base/` does not include `feature/`, and `feature/` does not include `app/`.

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
3. **Headers are contracts** — prefer heavy includes in `.cpp` files; keep headers lean to limit compile-time coupling.
4. **Cross-controller wiring stays in app** — tool registration, tab ticks, and `ActionRouter` model-dirty callbacks belong in `src/app/`, not feature headers.
5. **Fork glue stays at the edge** — RmlUi via `pp_rmlui_backend` in `ui/`, `chat/`, and `ai/bindings/`; libp2p public API via `src/libp2p/fork/include/` in `messaging/`.

---

## Current state

The dependency hierarchy above is **enforced at the header level** for upward feature-module edges. Regressions are caught by [`scripts/check_feature_includes.sh`](../../scripts/check_feature_includes.sh).

### Cycle-breaking patterns

| Pattern | Location | Purpose |
|---------|----------|---------|
| `ChatSessionPorts` | `ui/ChatSessionPorts.h` | Injected chat nav ports for contacts/people-picker; Application fills from `ChatController` — no singleton |
| `SettingsCommands` | `settings/SettingsCommands.h` | All settings cross-module ports (member on `SettingsController`); Application binds — no messaging bind / no extra singleton |
| `ProfileIdentityView` | `base/people/ProfileIdentityView.h` | Shared identity presentation DTO (filled by `MessagingHub`) |
| SessionStore listeners + nested service slices | `SessionStore`, nested `*::Apply` types, `ConfigApplyBridge` | Settings flush persists disk DTOs; app projects slices so settings UI does not own service apply |
| Hub-and-spoke within messaging | `MessagingHub` referenced from `MessageRouter`, `InboxController`, etc. | Orchestration inside single target `pp_feature_messaging` (compile coupling, not a link-cycle) |
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
| `rmlui_unit_tests` | doctest + RmlUi fork | [`src/render/fork/Tests/`](../render/fork/Tests/); includes fork `ClickRouting` cases |

Place tests at the **highest layer they include or link** (see SRC_LAYOUT). Base tests must not depend on `pp_feature`.

**Where active development lives**

| Area | Feature role | Base / project pointer |
|------|--------------|------------------------|
| Agent turns | `AgentSession`, turn pipeline | `base/ai/` (LlmClient, TurnPlan, conversation) |
| P2P messaging | `MessagingHub`, sync, relay | [`docs/architecture/P2P_MESSAGING.md`](../../docs/architecture/P2P_MESSAGING.md) |
| Window shell | `ShellHost`, document loading | [`docs/ui/WINDOW_SHELL.md`](../../docs/ui/WINDOW_SHELL.md) |
| Chat UI | `ChatController`, messaging tools | `assets/views/chat.rml`, `base/messaging/` stores |
| Settings | Section handlers, config merge | `assets/views/settings.rml`, `base/data/Config.h` |
| At-rest PIN gate | `PinGateController` | [`projects/at-rest-crypto/`](../../projects/at-rest-crypto/) |

---

## Adding or changing code

1. Find the module that **owns** the screen, session, or multi-module workflow.
2. Follow the dependency principles above; respect CMake `PUBLIC_LIBS` order in the owning folder's `CMakeLists.txt`.
3. Add tests in `src/feature/<module>/tests/` (`*_test.cpp` files). One executable per folder is created automatically (`pp_browser_feature_<module>_test`).
4. Run [`scripts/check_feature_includes.sh`](../../scripts/check_feature_includes.sh) before pushing.
5. Document externally visible behavior in [`docs/contracts/`](../../docs/contracts/) or [`docs/ui/`](../../docs/ui/) when wire formats or UI contracts change.

---

## Further reading

| Doc | Why |
|-----|-----|
| [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) | Full four-layer layout, fork sidecars, test placement |
| [`docs/architecture/ARCHITECTURE.md`](../../docs/architecture/ARCHITECTURE.md) | System overview (SDL, RmlUi, agent, shell) |
| [`docs/architecture/P2P_MESSAGING.md`](../../docs/architecture/P2P_MESSAGING.md) | Messaging hub and P2P orchestration |
| [`docs/ui/WINDOW_SHELL.md`](../../docs/ui/WINDOW_SHELL.md) | Shell layout and document hosting |
| [`src/base/README.md`](../base/README.md) | Base-layer primitives and dependency rules |
| [`AGENTS.md`](../../AGENTS.md) | Agent-oriented map of the whole repo |
