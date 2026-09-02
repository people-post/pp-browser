# Source layout

**Tier:** architecture

`src/` is organized in **product layers** (folders). Dependencies flow downward only. See [independence rules](#independence-inside-a-layer) before adding includes.

**North Star sentence:** `common` names the shared language; `foundation` implements the shared kernel; `domain` implements independent product capabilities; `feature` composes them; `app` constructs the graph.

> **Migration note (paths):** Target top-level folders are `src/common/`, `src/foundation/`, `src/domain/`, `src/feature/`, `src/app/`. Foundation holds `runtime/`, `platform/`, `error/`, `i18n/`, `data/`, `crypto/`. Domain peers still live under `src/base/` with `#include "base/…"`. Domain peer allowlist is empty.

## Layers

| Layer | Target path | Role |
|-------|-------------|------|
| Common (external) | FetchContent [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common) | App-independent utilities (logger, `ResultOrError`, `Module`, `WorkerPool`, serialize) — namespace `pp` |
| **Common (in-tree)** | [`src/common/`](../../src/common/) | pp-browser **helpers + contracts**: tiny utilities and shared vocabulary/seams any peer may use without linking another peer (`pp_pbr_common`) |
| Crypto (external) | FetchContent [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto) | libsodium + ML-KEM-768 / ML-DSA-65 natives + thin `pp::` wrappers; product wire helpers stay in foundation/domain crypto |
| UI (external) | FetchContent / sibling [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) | Hard-forked RmlUi + FreeType / HarfBuzz / LunaSVG + SDL3/GL3; product host/overlays stay in browser |
| Lib | [`src/lib/`](../../src/lib/) | Owned hard forks / extracted stacks (Amp via FetchContent; RmlUi via pp-cpp-ui); may use `third_party` (+ optionally `common`); not product domain |
| **Foundation** | [`src/foundation/`](../../src/foundation/) | Shared **kernel implementations** every domain peer may use (runtime, platform, data, error/i18n, crypto) |
| **Domain** | `src/domain/` *(today: subset of `src/base/`)* | Heavy **peer libs** (people, messaging, net, mesh, media, ai, ui, render) — single-purpose engines/stores/clients |
| Feature | [`src/feature/`](../../src/feature/) | Orchestration: hubs, sessions, screens; **wires** common contracts to concrete domain types |
| App | [`src/app/`](../../src/app/) | Composition root: `main`, `Application`, `Bootstrap` |

## Dependency rule

```
app → feature → domain → foundation → common → pp_common (FetchContent)
                 feature → foundation / common     (allowed)
                 domain  → common / lib            (contracts + forks)
```

Today’s include/link graph still uses the aggregate name **base** (`pp_base_*`, `#include "base/…"`):

```
app → feature → base → lib → pp_pbr_common → pp_common
```

`lib` and `common` may use `third_party`. No upward `#include` across layers.

**Lifetimes** (who may destroy whom) are separate from include layers — see [OWNERSHIP.md](OWNERSHIP.md).

## Independence inside a layer

Folders **are** layers. Default: modules **inside** a layer do not depend on each other.

| Layer | Inside-layer rule |
|-------|-------------------|
| **common** | Fully independent units (headers / small TUs). No cycles. Depend only on `pp_common` (+ `pp_crypto` for wire Base64) and other common headers. |
| **foundation** | **Exception:** small **ordered bands** (not peer-independent). Config/paths/crypto naturally stack. Documented below. |
| **domain** | **Strict independence.** Peers must not `#include` or `PUBLIC_LIBS`-link each other. Cross-need → contract/DTO in `common`; wire in `feature` (or `app` for lifetimes). |
| **feature** | Acyclic module order (see [`src/feature/README.md`](../../src/feature/README.md)). Prefer ports over reverse edges. |
| **app** | Single composition root. |

### Litmus tests

- **Common (FetchContent):** reusable in another project; no pp-browser domain types.
- **Common (in-tree):** shared language or seam; **no ownership** of durable state / I/O engines. “Could two domain peers need this name without owning the lifecycle?”
- **Foundation:** owned kernel every domain peer may use. Opens paths, holds config/session, crypto primitives, process runtime — **not** messaging policy or HTTP product clients.
- **Domain:** owns a store, client, codec, or engine; **heavy** product coding. Single purpose.
- **Feature:** coordinates multiple domain (and foundation) modules into a workflow or screen.
- **App:** exists only to run and wire the product.
- **Lib:** owned upstream-shaped library; no `foundation`/`domain`/`feature`/`app`.

## What `src/common` provides

Two buckets only:

| Bucket | Examples | Not here |
|--------|----------|----------|
| **Basics** | `ValueJson`, `SettledWait`, `CodedFailure`, `LengthPrefixedCodec`, `ByteRateLimiter`, `EmojiKey`, `PbrCompat`, startup timing | SQLite, curl, SDL, RmlUi, Amp |
| **Domain contracts** | Ports (`IThreadStore`-shaped APIs, blob/relay interfaces), shared ids/enums, narrow DTOs two+ peers must name | Full codecs, stores, hubs, UI ports |

Guardrails:

1. Common may include only `pp_common` / `pp_crypto` (Base64) / STL / other `common/` headers — **never** `base/*`, `foundation/*`, `domain/*`, `feature/*`.
2. Prefer pure headers for contracts (virtual iface + POD/DTO). No orchestration `.cpp`.
3. Promote a type to common only when a **second domain peer** must compile against it without linking the owner. Otherwise keep it in the owning foundation/domain module.

See [`src/common/README.md`](../../src/common/README.md).

## Foundation vs domain (today under `src/base/`)

### Foundation (ordered bands)

Downward only within foundation:

```
runtime, platform(_core)
  ↑
error, i18n
  ↑
data
  ↑
crypto
  (+ thin mesh_identity / PeerId if treated as kernel)
```

| Path (today) | Contents |
|--------------|----------|
| `foundation/runtime/` | Process runtime: `AppRuntime`, coordinator, `WorkerDispatch`, lifecycle, branding/version |
| `foundation/platform/` | OS adapters: paths, assets, credentials, notifications; SDL glue (no GL). See [PLATFORM_CODE.md](PLATFORM_CODE.md) |
| `foundation/error/` | App error categories on top of common |
| `foundation/i18n/` | Localization catalogs |
| `foundation/data/` | Config, session, profiles, schema (`BootstrapTypes.h`) |
| `foundation/crypto/` | E2E/at-rest crypto primitives, vault, KEM helpers (not messaging policy) |

Target path after move: `src/foundation/<module>/`.

### Domain (strict peers)

| Path (today) | Contents |
|--------------|----------|
| `base/people/` | Identity and contacts stores; presentation DTOs |
| `base/messaging/` | Thread types, SQLite/JSON stores, relay/group/E2E codecs |
| `base/net/` | HTTP client, service clients (no people/messaging policy) |
| `base/mesh/` | Product Amp glue: host, ports, reachability, L4 coordinators — [MESH.md](MESH.md) |
| `base/media/` | `CallMediaEngine` — capture/playback + HW H264 |
| `base/ai/` | LLM client, turn types, parsers; `conversation/`, `mcp/` sublibs |
| `base/ui/` | Theme, view catalog, shell/working-set types, input coordinator |
| `base/render/` | Product RmlUi host/overlays; reusable SDL/GL in pp-cpp-ui |

Target path after move: `src/domain/<module>/`.

**Domain rule:** `net` must not link `people`/`messaging`; `ai` must not link concrete messaging stores; cross-peer needs go through `common` contracts and `feature` wiring.

Historical **acyclic** edges inside today’s `base/` (to peel during migration): `crypto` → `adp` → `mesh_identity` → `mesh` → `people`; Mesh/link **tests** may link `pp_base_mesh_identity` without full `pp_base_mesh`. See [projects/adp/STACK.md](../../projects/adp/STACK.md) and [MESH.md](MESH.md).

Module maps and current CMake names: [`src/base/README.md`](../../src/base/README.md).

## Lib subtree (`src/lib/`)

| Path | Role |
|------|------|
| *(RmlUi)* | Hard fork in [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) (`rmlui/`); paths via `PP_LIB_RMLUI_*` |
| `lib/amp/` | AMP L1–L3 + link stack (FetchContent `pp-cpp-amp`; ADP, session, channel, PeerLinkManager) |

Path constants: [`src/lib/pp_lib_paths.cmake`](../../src/lib/pp_lib_paths.cmake); RmlUi via pp-cpp-ui `PpCppUi.cmake`.

### Domain / feature glue for forks

| Path (today) | Role |
|--------------|------|
| `base/render/platform/` | Mobile GL lifecycle helpers |
| `base/render/renderer/` | Product overlays (loupe, call video tiles) |
| `base/render/host/` | `BrowserHost` product `Backend::*` bootstrap |
| `base/mesh/` | `MeshHost`, reachability, L4 coordinators — see [MESH.md](MESH.md) |

```
domain/render → pp-cpp-ui `pp_ui` / PP_LIB_RMLUI_INCLUDE
domain/mesh → adp (+ mesh_identity)
feature/ui → domain/render
feature/messaging → domain/mesh
```

Product UI composition (`ShellHost`, `DocumentLoader`, `RmlMount`) stays in `src/feature/ui/`.

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
| `pp_base_*` | today’s foundation + domain modules (e.g. `pp_base_data`, `pp_base_mesh`) — rename to `pp_foundation_*` / `pp_domain_*` when folders move |
| `pp_base` | aggregate (`INTERFACE`; `pp_identity` is an alias) |
| `pp_feature_*` | feature — one static library per module folder |
| `pp_feature` | feature aggregate (`INTERFACE`) |
| `pp-browser` | app executable (defined in [`src/app/CMakeLists.txt`](../../src/app/CMakeLists.txt)) |

Base module tests compile to one executable per folder (e.g. `pp_browser_mesh_test`, `pp_browser_people_test`). Feature module tests use a `pp_browser_feature_<module>_test` prefix.

Fork product profiles: `src/lib/pp_lib_paths.cmake`, pp-cpp-ui `PpCppUi.cmake`. Shared `third_party` wiring: `cmake/dependencies.cmake`, `cmake/libp2p_dependencies.cmake`.

## Test placement

- Fork-level RmlUi tests live in pp-cpp-ui `rmlui/Tests/` and run in that repo’s CI (`PP_UI_BUILD_TESTS`), not under pp-browser ctest.
- Mesh glue tests live under [`src/base/mesh/tests/`](../../src/base/mesh/tests/).
- Keep integration and environment-heavy **pp-browser** tests outside the fork when they span app layers; colocate module unit tests under the owning module’s `tests/` and `src/feature/.../tests/`.
- Place a test with the **highest layer it includes or links** (foundation/domain tests must not depend on `pp_feature`).
- Module `CMakeLists.txt` files add `tests/` subdirectories when `PP_BROWSER_BUILD_TESTS` is on.

## Includes

Single include root: `${CMAKE_SOURCE_DIR}/src`. Use layer-prefixed paths:

```cpp
#include "common/ValueJson.h"
#include "foundation/data/Config.h"              // foundation (today)
#include "base/mesh/host/MeshHost.h"       // domain (today)
#include "feature/chat/ChatController.h"
#include "app/Application.h"
```

After folder moves, prefer `foundation/…` and `domain/…`.

Fork public APIs use their upstream include style (`<RmlUi/...>`, `<amp/...>`), with include roots from render/mesh PUBLIC dirs.

### Prefer include over forward declaration

When a type lives in a **legal dependency** (same layer / lower layer / allowed feature-module edge), **`#include` its header** rather than forward-declaring it. Forward declarations are for cycle-breaking and illegal upward edges — not the default for every pointer or reference member.

| Prefer `#include` when… | Prefer forward declare when… |
|-------------------------|------------------------------|
| The type is in a lower layer (`feature` → `domain`/`foundation`/`common`, `app` → …) | Including would create an **upward** or **cyclic** edge |
| The type is on an **allowed** same-layer / intra-feature edge | Incomplete type is enough **and** the include would force a forbidden module edge |
| You need the full definition for members, nested types, or `sizeof` | Breaking a temporary compile cycle while a ports/DTO extraction is planned |

Examples: feature/app headers that hold `SessionStore*` should `#include "foundation/data/SessionStore.h"`, not `class SessionStore;`. Do **not** forward-declare lower-layer types just to keep a header “lean.”

Still keep headers focused: avoid pulling unrelated heavy trees when a small `*Types.h` / ports header already exists (e.g. `SettingsCommands`, `ChatSessionPorts`).

## Migration order (when coding starts)

1. Enforce domain peer bans in CI (`check_base_includes.sh` + `check_base_public_libs.sh` + legacy allowlists) for **new** edges. **Started:** peels into `common/{directory,thread,chat,media,ui}/` with thin thread headers + role ports (`IThreadCatalog` / `Transcript` / `Memory` / `Sync`); attachment upload/fetch helpers live in `feature/messaging`. Removed legacy edges include `mesh→people/net/media`, `ai→ui/messaging/net`, `messaging→net`, `ai→net`.
2. Extract remaining cross-peer types/helpers (`net`↔messaging/people stores and relay sign helpers).
3. Move foundation folders to `src/foundation/`; update includes/CMake. **Started:** `error/`, `i18n/`.
4. Move domain folders to `src/domain/`; drop aggregate “base” naming.
5. Feature/app wiring absorbs former base↔base orchestration.
