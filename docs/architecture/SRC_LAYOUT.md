# Source layout

**Tier:** architecture

`src/` is organized in five layers. See dependency rules before adding includes.

## Layers

| Layer | Path | Role |
|-------|------|------|
| Common (external) | FetchContent [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common) | App-independent utilities (logger, `ResultOrError`, `Module`, `WorkerPool`, serialize) — namespace `pp` |
| Common (in-tree) | [`src/common/`](../../src/common/) | pp-browser cross-module helpers (`pp_pbr_common`): JSON bridge (`ValueJson.h`), `PbrCompat.h`, async/sync utilities (`SettledWait`, `StartupTiming`), wire helpers (`LengthPrefixedCodec`), small algorithms (`ByteRateLimiter`, `EmojiKey`, `CodedFailure` template). Per-module error escalation rules: [CODED_FAILURE.md](../contracts/CODED_FAILURE.md). |
| Crypto | FetchContent [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto) (pinned tag) | libsodium + ML-KEM-768 / ML-DSA-65 natives + thin `pp::` wrappers (`pp_crypto`); product wire helpers stay in `base/crypto` |
| UI | FetchContent / sibling [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) | Hard-forked RmlUi + FreeType / HarfBuzz / LunaSVG + SDL3/GL3 (`pp_ui` = `pp_ui_rml` + `pp_ui_backend`); product host/overlays stay in browser |
| Lib | [`src/lib/`](../../src/lib/) | Owned hard forks (RmlUi via pp-cpp-ui); may use `third_party` (+ optionally `common`); not product domain |
| Base | [`src/base/`](../../src/base/) | pp-browser primitives: runtime, platform, mesh/render glue, data, people, messaging/ai/ui |
| Feature | [`src/feature/`](../../src/feature/) | Composed capabilities: chat, agent session, shell, messaging hub |
| App | [`src/app/`](../../src/app/) | Composition root: `main`, `Application`, `Bootstrap` |

## Dependency rule

```
app → feature → base → lib → pp_pbr_common → pp_common (FetchContent)
```

`lib` and `common` may use `third_party`. No upward `#include` across layers.

**Lifetimes** (who may destroy whom) are separate from include layers — see [OWNERSHIP.md](OWNERSHIP.md).

## Lib subtree (`src/lib/`)

| Path | Role |
|------|------|
| *(RmlUi)* | Hard fork in [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) (`rmlui/`); paths via `PP_LIB_RMLUI_*` |
| `lib/amp/` | AMP L1–L3 + link stack (FetchContent `pp-cpp-amp`; ADP, session, channel, PeerLinkManager) |

Path constants: [`src/lib/pp_lib_paths.cmake`](../../src/lib/pp_lib_paths.cmake); RmlUi via pp-cpp-ui `PpCppUi.cmake`.

## Base glue for forks

| Path | Role |
|------|------|
| `base/render/platform/` | Mobile GL lifecycle helpers |
| `base/render/renderer/` | Product overlays (loupe, call video tiles) |
| `base/render/host/` | `BrowserHost` product `Backend::*` bootstrap |
| `base/mesh/` | `MeshHost`, reachability, L4 coordinators — see [MESH.md](MESH.md) |

Dependency rule:

```
base/render → pp-cpp-ui `pp_ui` (`pp_ui_rml` + `pp_ui_backend`) / PP_LIB_RMLUI_INCLUDE
base/mesh → adp (+ pp_base_mesh_identity)
feature/ui → base/render
feature/messaging → base/mesh
```

Product UI composition (`ShellHost`, `DocumentLoader`, `RmlMount`) stays in `src/feature/ui/`.

## Base subfolders

| Path | Contents |
|------|----------|
| `base/runtime/` | Process runtime: `AppRuntime`, coordinator, `WorkerDispatch`, lifecycle, branding/version |
| `base/platform/` | Cross-cutting OS adapters: SDL glue, paths, assets, credentials, notifications (no GL). Domain backends (codecs, sockets) stay with their module — [PLATFORM_CODE.md](PLATFORM_CODE.md) |
| `base/mesh/` | Product Amp glue: `host/` (MeshHost, MeshPorts), `identity/` (PeerId), `reachability/`, `l4/` coordinators |
| `lib/amp/L1/` | Association Datagram Protocol (Asio-free UDP L1: HMAC bind, path migrate, BE+reliable); no libp2p |
| `lib/amp/L2/` | AMP L2 — MSH, Session AEAD, rekey (`pp_base_mesh_session`) |
| `lib/amp/link/` | AMP link — `PeerLinkManager`, `MeshRuntime`, `AmpStack`, MSH-over-ADP (`pp_base_mesh_link`) |
| `lib/amp/L3/` | AMP L3 — channel mux, fragmentation, `ChannelSession` (`pp_base_mesh_channel`; [AMP-CHANNEL.md](../contracts/AMP-CHANNEL.md)) |
| `base/render/` | Product RmlUi host/overlays (`pp_base_render`); reusable SDL/GL in pp-cpp-ui `backend/` |
| `base/net/` | HTTP client, service clients |
| `base/data/` | Config, session, profiles, schema (`BootstrapTypes.h`) |
| `base/people/` | Identity and contacts stores; `ProfileIdentityView` presentation DTO |
| `base/messaging/` | Thread types, JSON store, parsers, reaction helpers |
| `base/media/` | `CallMediaEngine` — Opus + SDL capture/playback + colocated platform HW H264 |
| `base/ai/` | LLM client, turn types, parsers, conversation, MCP client |
| `base/ui/` | Theme, view catalog, shell/working-set types, input coordinator |

Acyclic order (excerpt): `crypto` → `adp` → `mesh_identity` → `mesh` → `people`. `adp` and `mesh` must not link `people`. Mesh/link **tests** may link `pp_base_mesh_identity` without full `pp_base_mesh`. See [projects/adp/STACK.md](../../projects/adp/STACK.md) and [MESH.md](MESH.md).

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
| `pp_common` | common (FetchContent) |
| `pp_pbr_common` | common (in-tree) |
| `pp_base_*` | base — one static library per module folder (e.g. `pp_base_data`, `pp_base_mesh`, `pp_base_render`) |
| `pp_base` | base aggregate (`INTERFACE`; `pp_identity` is an alias) |
| `pp_feature_*` | feature — one static library per module folder |
| `pp_feature` | feature aggregate (`INTERFACE`) |
| `pp-browser` | app executable (defined in [`src/app/CMakeLists.txt`](../../src/app/CMakeLists.txt)) |

Base module tests compile to one executable per folder (e.g. `pp_browser_mesh_test`, `pp_browser_people_test`). Feature module tests use a `pp_browser_feature_<module>_test` prefix.

Fork product profiles: `src/lib/pp_lib_paths.cmake`, pp-cpp-ui `PpCppUi.cmake`. Shared `third_party` wiring: `cmake/dependencies.cmake`, `cmake/libp2p_dependencies.cmake` (BoringSSL/zlib/Asio only; qtils/soralog/yaml-cpp removed with libp2p fork).

## Test placement

- Fork-level RmlUi tests live in pp-cpp-ui `rmlui/Tests/` and run in that repo’s CI (`PP_UI_BUILD_TESTS`), not under pp-browser ctest.
- Mesh glue tests live under [`src/base/mesh/tests/`](../../src/base/mesh/tests/).
- Keep integration and environment-heavy **pp-browser** tests outside the fork when they span app layers; colocate module unit tests under `src/base/.../tests/` and `src/feature/.../tests/`.
- Place a test with the **highest layer it includes or links** (base tests must not depend on `pp_feature`).
- Module `CMakeLists.txt` files add `tests/` subdirectories when `PP_BROWSER_BUILD_TESTS` is on.

## Litmus tests

- **Common (FetchContent):** reusable in another project; no pp-browser domain types.
- **Common (in-tree):** cross-module pp-browser helpers with no domain ownership; may depend on `pp_common` only.
- **Lib:** owned upstream-shaped library; no `base`/`feature`/`app`.
- **Base:** product-specific but single-purpose (one store, one client, one parser, or one glue module).
- **Feature:** coordinates multiple base modules into a workflow or screen.
- **App:** exists only to run and wire the product.

## Includes

Single include root: `${CMAKE_SOURCE_DIR}/src`. Use layer-prefixed paths:

```cpp
#include "common/Logger.h"
#include "base/data/Config.h"
#include "base/mesh/host/MeshHost.h"
#include "feature/chat/ChatController.h"
#include "app/Application.h"
```

Fork public APIs use their upstream include style (`<RmlUi/...>`, `<amp/...>`), with include roots from `pp_base_render` / `pp_base_mesh` PUBLIC dirs.

### Prefer include over forward declaration

When a type lives in a **legal dependency** (same layer / lower layer / allowed feature-module edge), **`#include` its header** rather than forward-declaring it. Forward declarations are for cycle-breaking and illegal upward edges — not the default for every pointer or reference member.

| Prefer `#include` when… | Prefer forward declare when… |
|-------------------------|------------------------------|
| The type is in a lower layer (`feature` → `base`/`common`, `app` → `feature`/`base`) | Including would create an **upward** or **cyclic** edge |
| The type is on an **allowed** same-layer / intra-feature edge (see above) | Incomplete type is enough **and** the include would force a forbidden module edge |
| You need the full definition for members, nested types, or `sizeof` | Breaking a temporary compile cycle while a ports/DTO extraction is planned |

Examples: feature/app headers that hold `SessionStore*` should `#include "base/data/SessionStore.h"`, not `class SessionStore;`. Do **not** forward-declare lower-layer types just to keep a header “lean.”

Still keep headers focused: avoid pulling unrelated heavy trees when a small `*Types.h` / ports header already exists (e.g. `SettingsCommands`, `ChatSessionPorts`).
