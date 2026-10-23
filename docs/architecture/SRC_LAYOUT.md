# Source layout

**Tier:** architecture

`src/` is organized in **product layers** (folders). Dependencies flow downward only. See [independence rules](#independence-inside-a-layer) before adding includes.

**North Star sentence:** `common` names the shared language; `foundation` implements the shared kernel; `lib` implements independent product capabilities; `domain` combines them into capabilities features share; `feature` composes those into what the user does; **`gui` presents** them; `app` constructs the graph.

> **Paths:** Top-level folders are `src/common/`, `src/foundation/`, `src/lib/`, `src/domain/`, `src/feature/`, `src/gui/`, `src/app/`. Foundation bands use `#include "foundation/…"`, `pp_foundation_*`. Lib peers use `#include "lib/…"`, `pp_lib_*` (aggregate `pp_lib` in [`lib/CMakeLists.txt`](../../src/lib/CMakeLists.txt)). Domain modules use `#include "domain/…"`, `pp_domain_*` (aggregate `pp_domain` in [`domain/CMakeLists.txt`](../../src/domain/CMakeLists.txt)). GUI uses `#include "gui/…"`, `pp_gui`. Convenience `pp_base` (foundation + Amp + `pp_lib`) lives in [`src/CMakeLists.txt`](../../src/CMakeLists.txt).

## Layers

| Layer | Target path | Role |
|-------|-------------|------|
| Common (external) | FetchContent [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common) | App-independent utilities (logger, `ResultOrError`, `Module`, `WorkerPool`, serialize) — namespace `pp` |
| **Common (in-tree)** | [`src/common/`](../../src/common/) | pp-browser **helpers + contracts**: tiny utilities and shared vocabulary/seams any peer may use without linking another peer (`pp_pbr_common`) |
| Crypto (external) | FetchContent [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto) | libsodium + ML-KEM-768 / ML-DSA-65 natives + thin `pp::` wrappers; product wire helpers stay in foundation/lib crypto |
| UI (external) | FetchContent / sibling [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) | Hard-forked RmlUi + FreeType / HarfBuzz / LunaSVG + SDL3/GL3; product host/overlays stay in browser |
| **Foundation** | [`src/foundation/`](../../src/foundation/) | Shared **kernel implementations** every lib peer may use (runtime, platform, data, error/i18n, crypto, identity/PeerId) |
| **Lib** | [`src/lib/`](../../src/lib/) | Heavy **peer libs** (people, messaging, net, mesh, media, ai, ui) — single-purpose engines/stores/clients |
| **Domain** | [`src/domain/`](../../src/domain/) | **Shared capabilities** composed from libs: registration, node services, call media, peers, attachments — no hub / session lifecycle, Rml-free |
| Feature | [`src/feature/`](../../src/feature/) | **Top-level features**: hubs, sessions, presenters' decisions; **wires** common contracts to concrete lib / domain types (Rml-free at module edge) |
| **GUI** | [`src/gui/`](../../src/gui/) | Product Rml/SDL presenters + shell ([F008](../../projects/feature-layer-reorg/DECISIONS.md#f008--gui-layer-above-feature)); name **gui** avoids clash with `lib/ui` |
| App | [`src/app/`](../../src/app/) | Composition root: `main`, `Application`, `Bootstrap` |

## Dependency rule

```
app → gui → feature → domain → lib → foundation → common → pp_common (FetchContent)
          gui     → feature / domain / lib / foundation / common
          feature → domain / lib / foundation / common     (no gui)
          domain  → lib / foundation / common               (no feature)
          lib     → foundation / common
```

Convenience link aggregate (not a folder layer):

```
pp_base INTERFACE = pp_foundation + pp_amp_{l1,l2,l3,link} + pp_lib
```

Foundation uses `pp_foundation_*` and `#include "foundation/…"`. Lib uses `pp_lib_*` and `#include "lib/…"`. Domain uses `pp_domain_*` and `#include "domain/…"`; each module names what it links (`pp_browser_add_domain_library`, no `pp_base`). Amp uses FetchContent targets `pp_amp_*`.

```
app → gui → feature → (pp_base, pp_domain) → domain → lib / foundation / amp → pp_pbr_common → pp_common
```

`common` may use `third_party`. No upward `#include` across layers.

**Lifetimes** (who may destroy whom) are separate from include layers — see [OWNERSHIP.md](OWNERSHIP.md).

## Independence inside a layer

Folders **are** layers. Default: modules **inside** a layer do not depend on each other.

| Layer | Inside-layer rule |
|-------|-------------------|
| **common** | Fully independent units (headers / small TUs). No cycles. Depend only on `pp_common` (+ `pp_crypto` for wire Base64) and other common headers. |
| **foundation** | **Exception:** small **ordered bands** (not peer-independent). Config/paths/crypto naturally stack. Documented below. |
| **lib** | **Strict independence.** Peers must not `#include` or `PUBLIC_LIBS`-link each other. Cross-need → contract/DTO in `common`; wire in `feature` (or `app` for lifetimes). |
| **domain** | **Ordered**, acyclic: a module uses only the ones before it in [`src/domain/CMakeLists.txt`](../../src/domain/CMakeLists.txt) (`check_domain_includes.sh` holds the order). |
| **feature** | Acyclic module order (see [`src/feature/README.md`](../../src/feature/README.md)). Prefer ports over reverse edges. Must not include `gui/`. |
| **gui** | May use feature/lib/foundation; must not include `app/`. Bands share `pp_gui` for now. |
| **app** | Single composition root. |

### Litmus tests

- **Common (FetchContent):** reusable in another project; no pp-browser product types.
- **Common (in-tree):** shared language or seam; **no ownership** of durable state / I/O engines. “Could two lib peers need this name without owning the lifecycle?”
- **Foundation:** owned kernel every lib peer may use. Opens paths, holds config/session, crypto primitives, process runtime — **not** messaging policy or HTTP product clients.
- **Lib:** owns a store, client, codec, or engine; **heavy** product coding. Single purpose.
- **Domain:** combines two or more libs into a capability, or is used by two or more features — and owns **no user-facing lifecycle** (no hub, session, screen). "Would a second feature want this without the first one's hub?" Yes → domain. It owns a hub / session / screen → feature.
- **Feature:** a top-level thing the user does (conversations, a call, a broadcast, the agent, settings): its hub / session and its screen's decisions, composed from domain and lib.
- **App:** exists only to run and wire the product.

## What `src/common` provides

Two buckets only:

| Bucket | Examples | Not here |
|--------|----------|----------|
| **Basics** | `LengthPrefixedCodec`, `ByteRateLimiter`, `EmojiKey`, `PbrCompat` (pp-cpp-common's `ValueJson`, `SettledWait`, `CodedFailure`, `AtomicFileWrite`, `MetricsRegistry` in `pbr`), `ArgsText`, `AttachmentDownloadPolicy`, startup timing | SQLite, curl, SDL, RmlUi, Amp |
| **Domain contracts** | Ports (`IThreadStore`-shaped APIs, blob/relay interfaces), shared ids/enums, narrow DTOs two+ peers must name | Full codecs, stores, hubs, UI ports |

Guardrails:

1. Common may include only `pp_common` / `pp_crypto` (Base64) / STL / other `common/` headers — **never** `foundation/*`, `lib/*`, `feature/*`.
2. Prefer pure headers for contracts (virtual iface + POD/DTO). No orchestration `.cpp`.
3. Promote a type to common only when a **second lib peer** must compile against it without linking the owner. Otherwise keep it in the owning foundation/lib module.

See [`src/common/README.md`](../../src/common/README.md).

## Foundation vs lib

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
  (+ identity / PeerId as kernel)
```

| Path | Contents |
|------|----------|
| `foundation/runtime/` | Process runtime: `AppRuntime`, coordinator, `WorkerDispatch`, lifecycle, branding/version; owner threads and a runner's executor on one (`RuntimeOwnerExecutor`) |
| `foundation/platform/` | OS adapters: paths, assets, credentials, notifications; `platform/ui/` = window host + RmlUi Backend + overlays. See [PLATFORM_CODE.md](PLATFORM_CODE.md) |
| `foundation/error/` | App error categories on top of common |
| `foundation/i18n/` | Localization catalogs |
| `foundation/data/` | Config, session, profiles, schema (`BootstrapTypes.h`) |
| `foundation/crypto/` | E2E/at-rest crypto primitives, vault, KEM helpers (not messaging policy) |
| `foundation/identity/` | PeerId helpers for product code (`MultiaddrPeerId.h`); the derivation is pp-cpp-crypto's (`crypto/PeerId.h`) |

### Lib (strict peers)

| Path | Contents |
|------|----------|
| `lib/people/` | Identity and contacts stores; presentation DTOs; registration classify (`RegistrationStatus`); link-device and local account backup (`LinkDeviceCoordinator`, `LocalAccountBackup`); a peer's Brief route (`PeerBriefRoute`, `ContactRelayRoute`); the name a thread or sender shows (`PeerDisplayResolver`; the directory cache and group titles through `PeerDisplaySources`) |
| `lib/messaging/` | Thread types, SQLite/JSON stores, relay/group/E2E codecs; the inbound receive engine (`RelayReceivePipeline`: verify, replay, decrypt, membership, control lane; the local keys through a `LocalKeys` port) and the group invite gate (`GroupInviteGate`; the inviter's standing through a port); the control lane (`ControlLaneEnvelope`, `ControlLaneSeenStore`); what a send of ours writes and puts on the wire (`OutboundMessage`: the row, the sealed payload, the envelope); what a transcript shows (`TranscriptAssembly`; the GUI draws it); a peer's DM (`PeerThreadLogic`); attachment prepare; chat attachment policy (views, posters, suppression, the RAM plaintext cache) — features wire it to `lib/content` |
| `lib/calls/` | Call vocabulary (`CallTypes`, `CallLifecycleTypes`), control codec and its system-control family (`CallControlFamily`), session and media-key stores and what a call's rows say (`CallSessionQueries`: who is in it, a peer's roster applied), the media key exchange (`CallMediaKeyExchange`, its control-lane sends through a port), the call-to-media seat (`CallMediaSeat`), the calls on this device and what they show (`LiveCalls`: call state, media Status, path gates), pure path / hop / migration logic (`Call*Logic`, `CallPathPolicy`, `CallPathMobility`, `SoftMigrateLogic`, `PeerCapsLogic`). No messaging include: messaging carries call control as a system-control family |
| `lib/broadcast/` | Live broadcast: join tickets, viewer ladders, relay media, broadcast RPC codec; the signed peer-announce feed (`PeerAnnounce*`, `Announce*`) |
| `lib/pricing/` | Initiation billing and payment promises: codecs, stores, lifecycle, and their system-control families (`PricingControlFamilies`) |
| `lib/content/` | The profile's content store ([C012](../../projects/content-cas/DECISIONS.md#c012--module-home-stay-in-messaging-until-public-cas-has-a-second-owner)): content-addressed private and public realms (`CasStore`, `ObjectIndex`, `CasTypes`), the library (`CasLibrary`), private content by hash (`PrivateContent`). Messaging never includes it: features wire attachments to it (`domain/attachments/AttachmentContent`, `ChatBlobContentSource`) |
| `lib/net/` | HTTP client, service clients (no people/messaging policy) |
| `lib/media/` | `CallMediaEngine` — capture/playback + HW H264 |
| `lib/ai/` | LLM client, turn types, parsers, payload plan builder; turn planning and the Brief turn runner (`TurnPlanner`, `BriefTurnRunner`); tool permissions and their prompt (`ToolPermissionPolicy`, `ToolPermissionPrompt`, `ParkedApproval`); `conversation/`, `mcp/` and `tools/` (web search, MCP tool adapters, the registry built from them) sublibs |
| `lib/ui/` | Product shell: theme, catalogs, input, context menu, `ShellLayout`, `ShellInterruption`, `ShellGestureAxis`, calendar/form helpers, `UiEditSession`, chat widget config builders |

Window host / Backend / overlays: `foundation/platform/ui/` (not a lib peer).

Module maps: [`src/foundation/README.md`](../../src/foundation/README.md), [`src/lib/README.md`](../../src/lib/README.md).

**Lib rule:** `net` must not link `people`/`messaging`; `ai` must not link concrete messaging stores; cross-peer needs go through `common` contracts and `feature` wiring.

Amp L1–L3 + link are FetchContent [`pp-cpp-amp`](https://github.com/people-post/pp-cpp-amp) targets (`pp_amp_l1` … `pp_amp_link`). The mesh on top of it — host, ports, reachability, L4 protocols — is FetchContent [`pp-cpp-mesh`](https://github.com/people-post/pp-cpp-mesh) (`pp_mesh`, namespace `pp::mesh`, `#include "mesh/…"`), composed by `feature/` and `app/`. App code spells it `mesh::X` (`common/MeshAlias.h`); `common/` / `foundation/` / `lib/` may include only its vocabulary, `mesh/types/` (`pp_mesh_types`, via `pp_pbr_common`). See [MESH.md](MESH.md).

## Forks and their glue

RmlUi is a hard fork in [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) (`rmlui/`); Amp L1–L3 + link are [`pp-cpp-amp`](https://github.com/people-post/pp-cpp-amp) (both FetchContent). Their product glue:

| Path | Role |
|--------------|------|
| `foundation/platform/ui/gl/` | Mobile GL lifecycle helpers |
| `foundation/platform/ui/renderer/` | Product overlays (loupe, call video tiles) |
| `foundation/platform/ui/host/` | `BrowserHost` product `Backend::*` bootstrap |

Product UI composition (`ShellHost`, `DocumentLoader`, `RmlMount`) lives under `src/gui/shell/` ([F008](../../projects/feature-layer-reorg/DECISIONS.md#f008--gui-layer-above-feature)).

## Domain subfolders

Module map and rules: [`src/domain/README.md`](../../src/domain/README.md). Order (a module uses only the ones above it):

| Path | Contents |
|------|----------|
| `domain/registration/` | Registration finish / renew / nickname (`pp_domain_registration`) — **headless**, linked by pp-node |
| `domain/node/` | Node-role mesh services (`pp_domain_node`): hosting from capabilities, DHT / directory config, self row, org-seed admission — **headless**, shared by pp-node and desktop `MeshRole::Node` |
| `domain/call_media/` | pp-browser's realtime protocol on the mesh (`pp_domain_call_media`): call-media transport, leg coordinator, frame crypto — a client of the mesh, not part of it; headless (pp-call-probe links it) |
| `domain/peers/` | Who a peer is (`pp_domain_peers`): directory nicknames and display names, relay-directory signing / KEM keys, profile icons; the people → messaging bridges |
| `domain/attachments/` | Chat attachments (`pp_domain_attachments`): blob request + mesh transport, CDN upload / fetch, background fetch + decrypt, content-store binding |

The **headless** ones build in `PP_BROWSER_HEADLESS` configures; pp-node's link closure is an allowlist ([BUILD.md § Headless mesh node](../ops/BUILD.md#headless-mesh-node-pp-node)). `src/app/node/` stays thin (main, bootstrap, env overlay, status HTTP); lab / smoke probes live in `src/app/node/tools/`.

## Feature subfolders

Module map, dependency rules, and test placement: [`src/feature/README.md`](../../src/feature/README.md).

| Path | Contents |
|------|----------|
| `feature/settings/` | Settings apply logic (no conversations/gui deps) |
| `feature/conversations/` | Conversations hub + delivery; never includes calls / broadcast / call_media — they ride the hub as `HubParticipant`s |
| `feature/product/` | `ProductStack` (`pp_feature_product`): owns the hub and registers calls (`CallsParticipant`) and broadcast (`BroadcastParticipant`) on it; only app/ and gui/ include it |
| `feature/calls/` | Call session (`pp_feature_calls`); delivery via ports |
| `feature/broadcast/` | Live broadcast (`pp_feature_broadcast`): `BroadcastHub`, viewer / broadcaster workflows; `BroadcastServices` (the announce feed, publisher, notifications and ticket server on the mesh — `AmpPeerAnnounceTransport`, `AmpBroadcastTransport`); never includes calls / conversations (sibling, [media-client-layers L001](../../projects/media-client-layers/DECISIONS.md)) |
| `feature/ai/` | AgentSession (the agent's runner), TurnExecutor (runs a plan's tools, people included), UI ports, bindings |

Feature module libraries stay acyclic. Conversations invoke AI through `AgentInboundPorts` (app-filled); `pp_feature_conversations` does not link `pp_feature_ai`. Calls invoke delivery through `CallDeliveryPorts` (hub-filled); `pp_feature_calls` does not include conversations:

```
settings
ai/tools → ai/bindings → ai
calls
conversations → calls
```

Feature modules use domain modules directly (`conversations → domain/registration, domain/node`; `calls → domain/call_media`).

## GUI subfolders

Module map: [`src/gui/README.md`](../../src/gui/README.md).

| Path | Contents |
|------|----------|
| `gui/` | Shared presenters (settings, call, pin, emoji, badges, flow, …) |
| `gui/shell/` | ShellHost, mount, shell ports |
| `gui/contacts/` | Contacts + people-picker |
| `gui/chat/` | ChatController + screen helpers |

`pp_gui` links `pp_feature_*` (and lib/foundation as needed). Feature must not link or include `gui/`.

Cross-controller wiring (tool registration, tab ticks, `ActionRouter` model dirty callbacks) lives in `src/app/`. Settings uses only `SettingsCommands` (declared in `feature/settings/`, bound on `SettingsController` from app) — no `BindMessaging`. Contacts/people-picker chat navigation uses injected `ChatSessionPorts` (filled from `ChatController` in app) without reversing the link graph.

## CMake targets

| Target | Layer |
|--------|-------|
| `pp_common` | common (FetchContent) |
| `pp_pbr_common` | common (in-tree) |
| `pp_foundation_*` | foundation modules under `src/foundation/` |
| `pp_foundation` | foundation aggregate (`INTERFACE`) |
| `pp_lib_*` | lib peers under `src/lib/` |
| `pp_lib` | lib aggregate (`INTERFACE` in [`lib/CMakeLists.txt`](../../src/lib/CMakeLists.txt); peers for this build) |
| `pp_amp_l1` / `pp_amp_l2` / `pp_amp_l3` / `pp_amp_link` | Amp (FetchContent pp-cpp-amp) |
| `pp_base` | convenience product stack (`INTERFACE` = foundation + Amp + `pp_lib`) |
| `pp_feature_*` | feature — one static library per module folder |
| `pp_feature` | feature aggregate (`INTERFACE`) |
| `pp_gui` | gui layer aggregate (presenters + shell) |
| `pp-browser` | app executable (defined in [`src/app/CMakeLists.txt`](../../src/app/CMakeLists.txt)) |

Foundation/lib module tests compile to one executable per folder (e.g. `pp_browser_people_test`). Feature module tests use a `pp_browser_feature_<module>_test` prefix.

Fork product profiles: pp-cpp-ui `PpCppUi.cmake`. Shared `third_party` wiring: `cmake/dependencies.cmake`, `cmake/libp2p_dependencies.cmake`.

## Test placement

- Fork-level RmlUi tests live in pp-cpp-ui `rmlui/Tests/` and run in that repo’s CI (`PP_UI_BUILD_TESTS`), not under pp-browser ctest.
- Mesh tests live in pp-cpp-mesh (`tests/`, its CI); pp-browser's tests that drive the mesh use its harnesses (`pp_mesh_testing`, `mesh/testing/…`).
- Keep integration and environment-heavy **pp-browser** tests outside the fork when they span app layers; colocate module unit tests under the owning module’s `tests/` and `src/feature/.../tests/`.
- Place a test with the **highest layer it includes or links** (foundation/lib tests must not depend on `pp_feature`).
- Module `CMakeLists.txt` files add `tests/` subdirectories when `PP_BROWSER_BUILD_TESTS` is on.

## Includes

Single include root: `${CMAKE_SOURCE_DIR}/src`. Use layer-prefixed paths:

```cpp
#include "common/ValueJson.h"
#include "foundation/data/Config.h"
#include "lib/people/ContactsStore.h"
#include "gui/chat/ChatController.h"
#include "app/Application.h"
```

Fork public APIs use their upstream include style (`<RmlUi/...>`, `<amp/...>`), with include roots from platform ui / Amp PUBLIC dirs; sibling libraries use their prefix (`"common/…"`, `"crypto/…"`, `"amp/…"`, `"mesh/…"`).

### Prefer include over forward declaration

When a type lives in a **legal dependency** (same layer / lower layer / allowed feature-module edge), **`#include` its header** rather than forward-declaring it. Forward declarations are for cycle-breaking and illegal upward edges — not the default for every pointer or reference member.

| Prefer `#include` when… | Prefer forward declare when… |
|-------------------------|------------------------------|
| The type is in a lower layer (`feature` → `lib`/`foundation`/`common`, `app` → …) | Including would create an **upward** or **cyclic** edge |
| The type is on an **allowed** same-layer / intra-feature edge | Incomplete type is enough **and** the include would force a forbidden module edge |
| You need the full definition for members, nested types, or `sizeof` | Breaking a temporary compile cycle while a ports/DTO extraction is planned |

Examples: feature/app headers that hold `SessionStore*` should `#include "foundation/data/SessionStore.h"`, not `class SessionStore;`. Do **not** forward-declare lower-layer types just to keep a header “lean.”

Still keep headers focused: avoid pulling unrelated heavy trees when a small `*Types.h` / ports header already exists (e.g. `SettingsCommands`, `ChatSessionPorts`).

### L4 protocols: `serve/` and `client/`

An Amp L4 protocol folder in pp-cpp-mesh (`l4/<protocol>/`, `dht/`, `discovery/`, `reachability/dial_back/`, `reachability/punch/`) keeps **shared wire types and pure decisions at its root** and splits the two ends of the protocol into sub-folders, so node-side code is easy to tell from the attaching side:

| Folder | Holds | Naming |
|--------|-------|--------|
| `<protocol>/` | Wire types, frame codecs, pure decision logic both ends use | `<Protocol>Types.h`, `<Protocol>Logic.*` |
| `<protocol>/serve/` | The side that answers inbound requests (what a node hosts) | `<Protocol>Server` (+ its state machines) |
| `<protocol>/client/` | The side that dials out and uses the service | `<Protocol>Client` (request / response), `<Protocol>ClientCoordinator` (session state), `I<Protocol>Client` / `Amp<Protocol>Client` (port + adapter) |
| `<protocol>/Amp<Protocol>` | Only when both ends share state — DHT record store, directory config, punch candidate addresses: a thin owner that composes the server and client | `AmpDhtProtocol`, `AmpDirectoryProtocol`, `AmpPunchCoordinator` |

Say **serve / server** for the answering side — not "host" (MeshHost, host sessions and `host_*` flags already mean other things). A client may use a co-located server directly (e.g. the media_relay local hop); the server never reaches into the client. Otherwise `MeshHost` owns the two ends directly (media_relay, circuit, dial-back). `call_media` is not split: both ends of a 1:1 media leg are peers running the same session.

### Free-function module names

Prefer a **topic / capability** filename (`AttachmentFetch`, `ShellLayout`, `ChatAttachmentPrepare`) over `*Util` / `*Helper` grab-bags. Keep `*Codec` / `*Json` / `*Cache` when that is the job. Rare `*Util` is OK for small pure shared bags (`CryptoUtil`). Place by layer ownership; merge only within one capability, never “one Utilities.cpp per folder.”

## Open layout work

Remaining moves and their history: [`projects/feature-layer-reorg/`](../../projects/feature-layer-reorg/) ([CURRENT_STATE](../../projects/feature-layer-reorg/CURRENT_STATE.md)).

## Related

- [TESTING.md](TESTING.md) — tiers mapped to these layers; push-down seams; where suite ledgers live
- [TEST_STRATEGY.md](../ops/TEST_STRATEGY.md) — purpose IDs, CI ladder, inventory
- [HARD_LAB.md](../../packaging/pp-node/HARD_LAB.md) — Tier C forced-hop / impairment ladder (design)
