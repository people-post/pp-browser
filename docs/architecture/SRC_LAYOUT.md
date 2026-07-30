# Source layout

**Tier:** architecture

`src/` is organized in four layers plus fork sidecars. See dependency rules before adding includes.

## Layers

| Layer | Path | Role |
|-------|------|------|
| Common | [`src/common/`](../../src/common/) | App-independent utilities (logger, `ResultOrError`, `SequencedTaskRunner`) |
| Base | [`src/base/`](../../src/base/) | pp-browser primitives: platform, data, people, messaging/ai/ui building blocks |
| Feature | [`src/feature/`](../../src/feature/) | Composed capabilities: chat, agent session, shell, messaging hub |
| App | [`src/app/`](../../src/app/) | Composition root: `main`, `Application`, `Bootstrap` |

**Fork sidecars** (not layers): [`src/render/`](../../src/render/), [`src/libp2p/`](../../src/libp2p/)

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
| `libp2p/fork/include/libp2p/host/explicit_host.hpp` | Preferred Host factory (no Boost.DI); app + muxer tests |
| `libp2p/fork/example/` | Sample programs (`PP_BROWSER_LIBP2P_EXAMPLES`); may still use DI injectors |
| `libp2p/fork/test/` | Unit tests (`PP_BROWSER_LIBP2P_TESTING` / coverage) |
| `libp2p/integration/host/` | `Libp2pHost` bootstrap glue (stub; chat history uses `createExplicitHost`) |

Dependency rule:

```
feature/messaging → fork/include (createExplicitHost)
integration/host → fork/include (public API only)
```

## Base subfolders

| Path | Contents |
|------|----------|
| `base/platform/` | SDL, paths, assets, threading (`BrowserThread`), credentials |
| `base/net/` | HTTP client, service clients |
| `base/data/` | Config, session, profiles, schema (`BootstrapTypes.h`) |
| `base/people/` | Identity and contacts stores; `ProfileIdentityView` presentation DTO |
| `base/messaging/` | Thread types, JSON store, parsers |
| `base/media/` | `CallMediaEngine` — libdatachannel + Opus + SDL voice media (a2) |
| `base/ai/` | LLM client, turn types, parsers, conversation, MCP client |
| `base/ui/` | Theme, view catalog, shell/working-set types, input coordinator |

## Feature subfolders

Module map, dependency rules, and test placement: [`src/feature/README.md`](../../src/feature/README.md).

| Path | Contents |
|------|----------|
| `feature/settings/` | Settings apply logic (no messaging/chat deps) |
| `feature/messaging/` | MessagingHub, router, inbox, P2P service |
| `feature/ai/` | AgentSession, turn pipeline, tools, bindings |
| `feature/ui/` | ShellHost, settings UI, profile/security sections; `ChatSessionPorts` + `SettingsCommands` (injected from app) |
| `feature/chat/` | Chat UI, agent↔hub wiring, messaging agent tools |

Feature module libraries link in acyclic order (each `PUBLIC_LIBS` only lower layers):

```
settings → ai/tools → ai/bindings → ai → messaging → ui → chat
```

Cross-controller wiring (tool registration, tab ticks, `ActionRouter` model dirty callbacks) lives in `src/app/`. Settings uses only `SettingsCommands` (declared in `feature/settings/`, bound on `SettingsController` from app) — no `BindMessaging`. Contacts/people-picker chat navigation uses injected `ChatSessionPorts` (filled from `ChatController` in app) without reversing the link graph.

## CMake targets

| Target | Layer |
|--------|-------|
| `pp_common` | common |
| `pp_base_*` | base — one static library per module folder (e.g. `pp_base_data`, `pp_base_messaging`) |
| `pp_base` | base aggregate (`INTERFACE`; `pp_identity` is an alias) |
| `pp_feature_*` | feature — one static library per module folder (e.g. `pp_feature_messaging`, `pp_feature_chat`) |
| `pp_feature` | feature aggregate (`INTERFACE`) |
| `pp_rmlui_backend` | render integration (SDL/GL glue; defined in [`src/render/CMakeLists.txt`](../../src/render/CMakeLists.txt)) |
| `pp_libp2p_integration` | libp2p integration glue (defined in [`src/libp2p/integration/CMakeLists.txt`](../../src/libp2p/integration/CMakeLists.txt)) |
| `pp-browser` | app executable (defined in [`src/app/CMakeLists.txt`](../../src/app/CMakeLists.txt)) |

Base module tests compile to one executable per folder (e.g. `pp_browser_data_test`, `pp_browser_messaging_test`). Feature module tests use a `pp_browser_feature_<module>_test` prefix (e.g. `pp_browser_feature_chat_test`) to avoid name clashes with base suites.

Fork-sidecar CMake helpers live in `cmake/PpBrowserRender.cmake` and `cmake/PpBrowserLibp2p.cmake` (mirroring `cmake/PpBrowserBase.cmake` and `cmake/PpBrowserFeature.cmake`).

## Test placement

- Fork-level RmlUi tests live in [`src/render/fork/Tests/`](../../src/render/fork/Tests/) (upstream doctest suite plus fork-specific `ClickRouting.cpp`).
- Keep integration and environment-heavy **pp-browser** tests outside the fork when they span app layers; colocate module unit tests under `src/base/.../tests/` and `src/feature/.../tests/`.
- Place a test with the **highest layer it includes or links** (base tests must not depend on `pp_feature`).
- Module `CMakeLists.txt` files add `tests/` subdirectories when `PP_BROWSER_BUILD_TESTS` is on; helpers live in `cmake/PpBrowserBase.cmake`, `cmake/PpBrowserFeature.cmake`, `cmake/PpBrowserRender.cmake`, and `cmake/PpBrowserLibp2p.cmake`.

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
