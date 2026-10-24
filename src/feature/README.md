# `src/feature`

pp-browser's **top-level features**: hubs, sessions, and the decisions of their screens, composed from [`src/domain/`](../domain/README.md) capabilities and [`src/lib/`](../lib/README.md) peers. Capabilities two features share live in `domain/`. Presenters / shell live in **`src/gui/`** ([F008](../../projects/feature-layer-reorg/DECISIONS.md#f008--gui-layer-above-feature)).

```
app → gui → feature → domain → lib → foundation → common → pp_common
feature    ← you are here
```

Includes use `foundation/…` and `lib/…` (see [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md)). Ongoing cleanup: [`projects/feature-layer-reorg/`](../../projects/feature-layer-reorg/).

**Rule:** dependencies flow downward only. Feature may use `foundation/`, `lib/`, and `common/`; it must not `#include` from `app/` or `gui/`. Runtime module wiring: [`docs/architecture/RUNTIME_COMPOSITION.md`](../../docs/architecture/RUNTIME_COMPOSITION.md).

Each top-level folder (and `ai/tools`, `ai/bindings`, plus band `conversations/calls/`) builds into **`pp_feature_<module>`** libraries. The aggregate **`pp_feature`** (`INTERFACE`) links them for app/`gui` code. Top-level `feature/chat`, `feature/ui`, and `feature/messaging` are **retired**.

---

## What belongs here

Feature code should **coordinate** multiple domain modules into a hub, session, or workflow — preferably headless-capable.

| Put it in feature when… | Put it in gui / domain / foundation when… |
|-------------------------|------------------------------------------|
| It coordinates several domain modules into a user flow | **gui:** owns documents, data models, chrome gestures |
| It **binds** `common` contracts to concrete types | **domain:** owns a store, client, or codec |
| It implements hub/session lifecycle without Rml | **foundation:** shared kernel every peer may link |

If you are unsure, ask: *"Could this exist without Rml?"* Yes → feature/lib. No → gui.

---

## Module map

Top-level folders (+ calls band). Two sub-trees under `ai/`.

```
src/feature/
├── settings/         Config apply logic, section handlers + SettingsTools
├── ai/               Agent session, turn pipeline, tools, bindings
│   ├── tools/
│   └── bindings/
├── calls/            Call session (`pp_feature_calls`)
├── broadcast/        Live broadcast viewer (`pp_feature_broadcast`) — sibling of calls, shared layers only
├── conversations/    Conversations hub + delivery; ConversationsFacade; `HubParticipant` (what rides the hub); `ChatPresenter` (the chats list's decisions + buttons, headless — ChatController renders it, pp-app-probe drives it)
└── product/          `ProductStack` (`pp_feature_product`): the hub with calls and broadcast as its participants — the one module that sees all three; only app/ and gui/ include it
```

Product UI: see [`src/gui/README.md`](../gui/README.md).

`IToolProvider` / `ToolRegistry` live in `lib/ai/` so settings and conversations can register tools without linking `pp_feature_ai`.

**Domain grouping (mental model):**

| Domain | Modules | Typical question it answers |
|--------|---------|----------------------------|
| **Configuration** | settings | How do user edits merge into `AppConfig`? |
| **Intelligence** | ai, ai/tools, ai/bindings | How does the agent plan turns and call tools? |
| **Call session** | calls | How do ring/accept/topology/media-bridge sessions run? |
| **Conversations** | conversations | How do threads sync/relay? |
| **Presentation** | `src/gui/` | Shell, contacts, chat screen, settings presenters |

Start points when exploring:

- Agent session → `ai/AgentSession.h`, `ai/TurnPlanner.h`, `ai/TurnExecutor.h`
- Conversations hub → `conversations/ConversationsHub.h`, `conversations/MeshDeliveryOrchestrator.h`
- Call session → `calls/CallStack.h`
- Window shell → `gui/shell/ShellHost.h`
- Chat screen → `gui/chat/ChatController.h`, `feature/conversations/MessagingTools.h`
- Settings apply → `settings/SettingsLogic.h`, `settings/SettingsSectionHandler.h`

Includes use the repo root: `#include "feature/conversations/ConversationsHub.h"`. GUI includes use `gui/…` from `src/gui/` / `src/app/` only.

---

## Dependency design

**Goal:** acyclic feature modules that link only lower layers, arranged so shared orchestration types live in the module that owns the workflow.

### Cross-layer direction

```
app → gui → feature → domain → lib → foundation → common
```

Feature modules link `pp_base` and `pp_common` (via `pp_browser_add_feature_library` in [`cmake/PpBrowserFeature.cmake`](../../cmake/PpBrowserFeature.cmake)), and the domain modules they use. Headless capabilities shared with pp-node (`registration`, `node`) are domain modules ([`src/domain/`](../domain/README.md)). Production code has no upward `#include` edges: `foundation/`/`lib/` do not include `feature/`, `feature/` does not include `gui/` or `app/`, and `gui/` does not include `app/`. Lib peers must not gain new edges to each other — peel those via `common` ports and feature wiring ([SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md)).

### Intra-feature direction

```
settings
ai/tools → ai/bindings → ai
calls
conversations → calls   (delivery / inbound ports; no reverse include)
```

Feature modules use domain modules directly (`conversations → domain/registration, domain/node`; `calls → domain/call_media`).

Conversations invoke AI through `AgentInboundPorts` (app-filled). Calls invoke delivery through `CallDeliveryPorts` (hub-filled). Calls bind their control messages as a system-control family (`common/chat/SystemControl.h`, [P2P_MESSAGING § System-control families](../../docs/architecture/P2P_MESSAGING.md#system-control-families)): the receive path, delivery and the inbox route by the registry, not by call types.

### Guards

- [`scripts/check/check_feature_includes.sh`](../../scripts/check/check_feature_includes.sh) — bans `feature → gui/app`, `conversations → feature/ai`, `calls → conversations`, retired paths (incl. the moved `feature/{registration,node,call_media}`)
- [`scripts/check/check_domain_includes.sh`](../../scripts/check/check_domain_includes.sh) — domain's order and downward edges
- [`scripts/check/check_gui_includes.sh`](../../scripts/check/check_gui_includes.sh) — bans `gui → app`

---

## Testing

Primary home for **integration** of typical valuable paths (real collaborators; fakes only at true ports). If a path is too expensive to cover here, prefer pushing a seam into lib/foundation rather than growing a mega-suite. Product multi-process purposes (`B-*`) live in [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md). Doctrine: [TESTING.md](../../docs/architecture/TESTING.md).
