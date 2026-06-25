# Source layout

`src/` is organized in four layers plus fork sidecars. See dependency rules before adding includes.

## Layers

| Layer | Path | Role |
|-------|------|------|
| Common | [`src/common/`](../src/common/) | App-independent utilities (logger, `ResultOrError`, `SequencedTaskRunner`) |
| Base | [`src/base/`](../src/base/) | pp-browser primitives: platform, data, people, messaging/ai/ui building blocks |
| Feature | [`src/feature/`](../src/feature/) | Composed capabilities: chat, agent session, shell, messaging hub |
| App | [`src/app/`](../src/app/) | Composition root: `main`, `Application`, `Bootstrap` |

**Fork sidecars** (not layers): [`src/render/`](../src/render/), [`src/libp2p/`](../src/libp2p/)

## Dependency rule

```
app → feature → base → common
```

No upward `#include` across layers. Forks are used at the base/feature boundary (`pp_rmlui_backend`, `libp2p/integration/`).

## Render subtree (`src/render/`)

| Path | Role |
|------|------|
| `render/fork/` | Upstream-shaped RmlUi hard fork (`Include/`, `Source/`, `CMake/`) |
| `render/fork/reference/backends/` | Upstream sample backends (reference only; not linked) |
| `render/integration/platform/` | SDL platform adapter |
| `render/integration/renderer/` | OpenGL3 render interface |
| `render/integration/host/` | `BrowserHost` bootstrap |

Dependency rule:

```
integration/host → integration/platform + integration/renderer → fork/Include (public API only)
```

Product UI composition (`ShellHost`, `DocumentLoader`, `RmlMount`) stays in `src/feature/ui/`.

## Base subfolders

| Path | Contents |
|------|----------|
| `base/platform/` | SDL, paths, assets, threading (`BrowserThread`), credentials |
| `base/net/` | HTTP client, service clients |
| `base/data/` | Config, session, profiles, schema (`BootstrapTypes.h`) |
| `base/people/` | Identity and contacts stores |
| `base/messaging/` | Thread types, JSON store, parsers |
| `base/ai/` | LLM client, turn types, parsers, conversation, MCP client |
| `base/ui/` | Theme, view catalog, shell/working-set types, input coordinator |

## Feature subfolders

| Path | Contents |
|------|----------|
| `feature/messaging/` | MessagingHub, router, inbox, P2P service |
| `feature/ai/` | AgentSession, turn pipeline, tools, bindings |
| `feature/ui/` | ShellHost, DocumentLoader, RmlMount, SettingsController |
| `feature/chat/` | Chat demo and helpers |
| `feature/search/` | Search demo |
| `feature/dynamic/` | Dynamic RML demo |
| `feature/settings/` | Settings apply logic |

## CMake targets

| Target | Layer |
|--------|-------|
| `pp_common` | common |
| `pp_base` | base (`pp_identity` is an alias) |
| `pp_feature` | feature |
| `pp-browser` | app executable |

## Litmus tests

- **Common:** reusable in another project; no pp-browser domain types.
- **Base:** product-specific but single-purpose (one store, one client, one parser).
- **Feature:** coordinates multiple base modules into a workflow or screen.
- **App:** exists only to run and wire the product.

## Includes

Single include root: `${CMAKE_SOURCE_DIR}/src`. Use layer-prefixed paths:

```cpp
#include "common/Logger.h"
#include "base/data/Config.h"
#include "feature/chat/ChatDemo.h"
#include "app/Application.h"
```
