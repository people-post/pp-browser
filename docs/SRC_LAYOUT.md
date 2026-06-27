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

## libp2p subtree (`src/libp2p/`)

| Path | Role |
|------|------|
| `libp2p/fork/` | Upstream-shaped cpp-libp2p hard fork (`include/`, `src/`, `cmake/`, `example/`, `test/`) |
| `libp2p/fork/example/` | Sample programs (`PP_BROWSER_LIBP2P_EXAMPLES`) |
| `libp2p/fork/test/` | Unit tests (`PP_BROWSER_LIBP2P_TESTING` / coverage) |
| `libp2p/integration/host/` | `Libp2pHost` bootstrap glue |

Dependency rule:

```
integration/host → fork/include (public API only)
```

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
| `feature/chat/` | Chat UI and helpers |
| `feature/settings/` | Settings apply logic |

## CMake targets

| Target | Layer |
|--------|-------|
| `pp_common` | common |
| `pp_base` | base (`pp_identity` is an alias) |
| `pp_feature` | feature |
| `pp-browser` | app executable |

## Test placement

- Keep integration and environment-heavy tests in [`tests/`](../tests/).
- Prefer colocated unit tests under module paths such as `src/base/.../tests/` and `src/feature/.../tests/`.
- Register module-local tests through CMake so they are discovered by CTest in regular desktop builds.

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
#include "feature/chat/ChatController.h"
#include "app/Application.h"
```
