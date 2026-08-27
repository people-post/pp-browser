# Source layout

**Tier:** architecture

`src/` is organized in five layers. See dependency rules before adding includes.

## Layers

| Layer | Path | Role |
|-------|------|------|
| Common | FetchContent [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common) | App-independent utilities (logger, `ResultOrError`, `Module`, `WorkerPool`, serialize) — namespace `pp`; browser bridge in [`src/common/PbrCompat.h`](../../src/common/PbrCompat.h) |
| Crypto | FetchContent / sibling [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto) | libsodium + ML-KEM-768 / ML-DSA-65 natives + thin `pp::` wrappers (`pp_crypto`); product wire helpers stay in `base/crypto` |
| Lib | [`src/lib/`](../../src/lib/) | Owned hard forks (RmlUi, libp2p); may use `third_party` (+ optionally `common`); not product domain |
| Base | [`src/base/`](../../src/base/) | pp-browser primitives: runtime, platform, p2p/render glue, data, people, messaging/ai/ui |
| Feature | [`src/feature/`](../../src/feature/) | Composed capabilities: chat, agent session, shell, messaging hub |
| App | [`src/app/`](../../src/app/) | Composition root: `main`, `Application`, `Bootstrap` |

## Dependency rule

```
app → feature → base → lib → common
```

`lib` and `common` may use `third_party`. No upward `#include` across layers.

## Lib subtree (`src/lib/`)

| Path | Role |
|------|------|
| `lib/rmlui/` | Upstream-shaped RmlUi hard fork (`Include/`, `Source/`, `CMake/`, `Tests/`) |
| `lib/rmlui/reference/backends/` | Upstream sample backends (reference only; not linked) |
| `lib/libp2p/` | Upstream-shaped cpp-libp2p hard fork (`include/`, `src/`, `cmake/`, `example/`, `test/`) |
| `lib/libp2p/include/libp2p/host/explicit_host.hpp` | Preferred Host factory (no Boost.DI) |

Path constants and product profiles: [`src/lib/pp_lib_paths.cmake`](../../src/lib/pp_lib_paths.cmake), [`src/lib/pp_lib_rmlui.cmake`](../../src/lib/pp_lib_rmlui.cmake), [`src/lib/pp_lib_libp2p.cmake`](../../src/lib/pp_lib_libp2p.cmake) (`PP_LIB_RMLUI_*`, `PP_LIB_LIBP2P_*`).

## Base glue for forks

| Path | Role |
|------|------|
| `base/render/platform/` | SDL platform adapter |
| `base/render/renderer/` | OpenGL3 render interface |
| `base/render/host/` | `BrowserHost` bootstrap |
| `base/p2p/` | `Libp2pHost`, mesh/relay/stream glue |

Dependency rule:

```
base/render → lib/rmlui/Include (public API only)
base/p2p → lib/libp2p/include (public API only)
feature/ui → base/render
feature/messaging → base/p2p
```

Product UI composition (`ShellHost`, `DocumentLoader`, `RmlMount`) stays in `src/feature/ui/`.

## Base subfolders

| Path | Contents |
|------|----------|
| `base/runtime/` | Process runtime: `AppRuntime`, coordinator, `WorkerDispatch`, `StartupTiming`, lifecycle, branding/version |
| `base/platform/` | Cross-cutting OS adapters: SDL glue, paths, assets, credentials, notifications (no GL). Domain backends (codecs, sockets) stay with their module — [PLATFORM_CODE.md](PLATFORM_CODE.md) |
| `base/p2p/` | Libp2p product glue (mesh, circuit/media relay, stream framing); OS net-if / mDNS sockets in `*_Win32.cpp` / `*_Posix.cpp` |
| `base/render/` | RmlUi SDL/GL backend (`pp_base_render`); GL/GLES in `render/platform/` |
| `base/net/` | HTTP client, service clients |
| `base/data/` | Config, session, profiles, schema (`BootstrapTypes.h`) |
| `base/people/` | Identity and contacts stores; `ProfileIdentityView` presentation DTO |
| `base/messaging/` | Thread types, JSON store, parsers, reaction helpers (`EmojiKey`) |
| `base/media/` | `CallMediaEngine` — Opus + SDL capture/playback + colocated platform HW H264 |
| `base/ai/` | LLM client, turn types, parsers, conversation, MCP client |
| `base/ui/` | Theme, view catalog, shell/working-set types, input coordinator |

Acyclic order (excerpt): `crypto` → `p2p` → `people`. `p2p` may include header-only `people/RelayScope.h` but must not link `pp_base_people`.

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
| `pp_base_*` | base — one static library per module folder (e.g. `pp_base_data`, `pp_base_p2p`, `pp_base_render`) |
| `pp_base` | base aggregate (`INTERFACE`; `pp_identity` is an alias) |
| `pp_feature_*` | feature — one static library per module folder |
| `pp_feature` | feature aggregate (`INTERFACE`) |
| `pp-browser` | app executable (defined in [`src/app/CMakeLists.txt`](../../src/app/CMakeLists.txt)) |

Base module tests compile to one executable per folder (e.g. `pp_browser_p2p_test`, `pp_browser_people_test`). Feature module tests use a `pp_browser_feature_<module>_test` prefix.

Fork product profiles (embedding policy + path constants): `src/lib/pp_lib_paths.cmake`, `src/lib/pp_lib_rmlui.cmake`, `src/lib/pp_lib_libp2p.cmake`. Shared `third_party` wiring stays in `cmake/dependencies.cmake` and `cmake/libp2p_dependencies.cmake`.

## Test placement

- Fork-level RmlUi tests live in [`src/lib/rmlui/Tests/`](../../src/lib/rmlui/Tests/) (upstream doctest suite plus fork-specific `ClickRouting.cpp`).
- Libp2p glue tests live under [`src/base/p2p/tests/`](../../src/base/p2p/tests/).
- Keep integration and environment-heavy **pp-browser** tests outside the fork when they span app layers; colocate module unit tests under `src/base/.../tests/` and `src/feature/.../tests/`.
- Place a test with the **highest layer it includes or links** (base tests must not depend on `pp_feature`).
- Module `CMakeLists.txt` files add `tests/` subdirectories when `PP_BROWSER_BUILD_TESTS` is on.

## Litmus tests

- **Common:** reusable in another project; no pp-browser domain types.
- **Lib:** owned upstream-shaped library; no `base`/`feature`/`app`.
- **Base:** product-specific but single-purpose (one store, one client, one parser, or one glue module).
- **Feature:** coordinates multiple base modules into a workflow or screen.
- **App:** exists only to run and wire the product.

## Includes

Single include root: `${CMAKE_SOURCE_DIR}/src`. Use layer-prefixed paths:

```cpp
#include "common/Logger.h"
#include "base/data/Config.h"
#include "base/p2p/Libp2pHost.h"
#include "feature/chat/ChatController.h"
#include "app/Application.h"
```

Fork public APIs use their upstream include style (`<RmlUi/...>`, `<libp2p/...>`), with include roots from `pp_base_render` / `pp_base_p2p` PUBLIC dirs.

### Prefer include over forward declaration

When a type lives in a **legal dependency** (same layer / lower layer / allowed feature-module edge), **`#include` its header** rather than forward-declaring it. Forward declarations are for cycle-breaking and illegal upward edges — not the default for every pointer or reference member.

| Prefer `#include` when… | Prefer forward declare when… |
|-------------------------|------------------------------|
| The type is in a lower layer (`feature` → `base`/`common`, `app` → `feature`/`base`) | Including would create an **upward** or **cyclic** edge |
| The type is on an **allowed** same-layer / intra-feature edge (see above) | Incomplete type is enough **and** the include would force a forbidden module edge |
| You need the full definition for members, nested types, or `sizeof` | Breaking a temporary compile cycle while a ports/DTO extraction is planned |

Examples: feature/app headers that hold `SessionStore*` should `#include "base/data/SessionStore.h"`, not `class SessionStore;`. Do **not** forward-declare lower-layer types just to keep a header “lean.”

Still keep headers focused: avoid pulling unrelated heavy trees when a small `*Types.h` / ports header already exists (e.g. `SettingsCommands`, `ChatSessionPorts`).
