# `src/feature`

Product **orchestration** for pp-browser: hubs, sessions, and workflows that **wire** domain peers (and foundation). Presenters / shell live in **`src/gui/`** ([F008](../../projects/feature-layer-reorg/DECISIONS.md#f008--gui-layer-above-feature)).

```
app → gui → feature → domain → foundation → common → pp_common
feature    ← you are here
```

Includes use `foundation/…` and `domain/…` (see [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md)). Ongoing cleanup: [`projects/feature-layer-reorg/`](../../projects/feature-layer-reorg/).

**Rule:** dependencies flow downward only. Feature may use `foundation/`, `domain/`, `common/`, and `lib/`; it must not `#include` from `app/` or `gui/`. Runtime module wiring: [`docs/architecture/RUNTIME_COMPOSITION.md`](../../docs/architecture/RUNTIME_COMPOSITION.md).

Each top-level folder (and `ai/tools`, `ai/bindings`, plus band `messaging/calls/`) builds into **`pp_feature_<module>`** libraries. The aggregate **`pp_feature`** (`INTERFACE`) links them for app/`gui` code. Top-level `feature/chat` and `feature/ui` are **retired**.

---

## What belongs here

Feature code should **coordinate** multiple domain modules into a hub, session, or workflow — preferably headless-capable.

| Put it in feature when… | Put it in gui / domain / foundation when… |
|-------------------------|------------------------------------------|
| It coordinates several domain modules into a user flow | **gui:** owns documents, data models, chrome gestures |
| It **binds** `common` contracts to concrete types | **domain:** owns a store, client, or codec |
| It implements hub/session lifecycle without Rml | **foundation:** shared kernel every peer may link |

If you are unsure, ask: *"Could this exist without Rml?"* Yes → feature/domain. No → gui.

---

## Module map

Three top-level folders (+ calls band). Two sub-trees under `ai/`.

```
src/feature/
├── settings/     Config apply logic, section handlers + SettingsTools
├── ai/           Agent session, turn pipeline, tools, bindings
│   ├── tools/
│   └── bindings/
└── messaging/    Conversations hub (legacy name) + delivery; MessagingFacade
    └── calls/    Call session band (f4v1 → future feature/calls)
```

Product UI: see [`src/gui/README.md`](../gui/README.md).

`IToolProvider` / `ToolRegistry` live in `domain/ai/` so settings and messaging can register tools without linking `pp_feature_ai`.

**Domain grouping (mental model):**

| Domain | Modules | Typical question it answers |
|--------|---------|----------------------------|
| **Configuration** | settings | How do user edits merge into `AppConfig`? |
| **Intelligence** | ai, ai/tools, ai/bindings | How does the agent plan turns and call tools? |
| **Conversations** | messaging (+ calls band) | How do threads sync/relay; how do call sessions run? |
| **Presentation** | `src/gui/` | Shell, contacts, chat screen, settings presenters |

Start points when exploring:

- Agent session → `ai/AgentSession.h`, `ai/TurnPlanner.h`, `ai/TurnExecutor.h`
- Conversations hub → `messaging/MessagingHub.h`, `messaging/MeshMessagingService.h`
- Call session → `messaging/calls/CallStack.h`
- Window shell → `gui/shell/ShellHost.h`
- Chat screen → `gui/chat/ChatController.h`, `gui/chat/MessagingTools.h`
- Settings apply → `settings/SettingsLogic.h`, `settings/SettingsSectionHandler.h`

Includes use the repo root: `#include "feature/messaging/MessagingHub.h"`. GUI includes use `gui/…` from `src/gui/` / `src/app/` only.

---

## Dependency design

**Goal:** acyclic feature modules that link only lower layers, arranged so shared orchestration types live in the module that owns the workflow.

### Cross-layer direction

```
app → gui → feature → domain → foundation → common
```

Feature modules always link `pp_base` and `pp_common` (via `pp_browser_add_feature_library` in [`cmake/PpBrowserFeature.cmake`](../../cmake/PpBrowserFeature.cmake)). Production code has no upward `#include` edges: `foundation/`/`domain/` do not include `feature/`, `feature/` does not include `gui/` or `app/`, and `gui/` does not include `app/`. Domain peers must not gain new edges to each other — peel those via `common` ports and feature wiring ([SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md)).

### Intra-feature direction

Intended link order (each `PUBLIC_LIBS` only lower feature modules):

```
settings → ai/tools → ai/bindings → ai → messaging
```

(`gui` is a separate layer that links `pp_feature_*` as needed.)

### Guards

- [`scripts/check_feature_includes.sh`](../../scripts/check_feature_includes.sh) — bans `feature → gui/app`, retired `feature/ui` and `feature/chat`
- [`scripts/check_gui_includes.sh`](../../scripts/check_gui_includes.sh) — bans `gui → app`
