# Agent guide for pp-browser

This document orients coding agents working in this repository.

## Architecture

pp-browser is a native AI-oriented UI shell:

- **SDL3 + OpenGL3** — product window host in `src/base/render/`; reusable Platform_SDL / Renderer_GL3 in [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui)
- **Hard-forked RmlUi** — UI layout via FetchContent / sibling [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) (fork sources live in that repo)
- **Hard-forked libp2p** — PeerId + key wire only in `src/lib/libp2p/` (A017; mesh underlay is Amp)
- **Third-party libs** — curl and shared deps in [`third_party/`](third_party/); JSON via [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common) (`Value`/`Object`); libsodium + PQ via [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto); RmlUi + FreeType / HarfBuzz / LunaSVG + SDL3 / SDL3_image via pp-cpp-ui
- **Five-layer source tree** — FetchContent `pp-cpp-common` + `pp-cpp-crypto` + `pp-cpp-ui` + `src/lib/`, `src/base/`, `src/feature/`, `src/app/` — see [docs/architecture/SRC_LAYOUT.md](docs/architecture/SRC_LAYOUT.md)

See [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) for the full picture. **UI ↔ functional boundary:** [docs/architecture/UI_FUNCTIONAL_BOUNDARY.md](docs/architecture/UI_FUNCTIONAL_BOUNDARY.md) (state / config / actions / events; app-owned presenters). **Networking:** [docs/architecture/NETWORKING.md](docs/architecture/NETWORKING.md) (HTTP + Amp mesh). Doc tiers: [docs/README.md](docs/README.md). Compatibility: [docs/contracts/COMPATIBILITY.md](docs/contracts/COMPATIBILITY.md).

## RmlUi is maintained in pp-cpp-ui

We **own and modify** the hard fork in sibling [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) (`rmlui/`). Consume via FetchContent / `../pp-cpp-ui`.

- Edit RmlUi in **pp-cpp-ui** when app-level workarounds are insufficient (layout, text selection, new properties, etc.).
- Document fork-specific changes in [docs/architecture/RMLUI_UPSTREAM.md](docs/architecture/RMLUI_UPSTREAM.md).
- App-specific host/overlays stay in [`src/base/render/`](src/base/render/); reusable SDL/GL backend lives in pp-cpp-ui `backend/`.

### Fork features (pp-browser)

| Feature | Location (in pp-cpp-ui) | Usage |
|---------|----------|--------|
| Text selection in static content | `rmlui/Source/Core/Elements/ElementSelectableText.*`, `SelectionController.*` | RML attribute `selectable="text"`; participation API on `Element`; Ctrl+C copies selection |
| User-agent baseline styles | `rmlui/Source/Core/UserAgentStyleSheet.*` | Auto-merged into every document; author RCSS overrides |
| List markers (workaround) | `rmlui/Source/Core/ListMarker.*`, `Layout/InlineLevelBox.cpp` | `ul`/`ol` bullets until `list-style` exists — see [RMLUI_UPSTREAM.md](docs/architecture/RMLUI_UPSTREAM.md) |

## libp2p is maintained in-tree (PeerId only)

We **own** the hard fork under [`src/lib/libp2p/`](src/lib/libp2p/). After **A017** it retains **PeerId + key wire** only (no Host/TCP/Yamux/Noise). Mesh/dial/mux lives in [`src/base/adp/`](src/base/adp/) + [`src/base/mesh/`](src/base/mesh/) + [`src/base/p2p/`](src/base/p2p/).

- Document fork changes in [docs/architecture/LIBP2P_UPSTREAM.md](docs/architecture/LIBP2P_UPSTREAM.md).
- Import/update remaining deps with `./scripts/libp2p_vendor_import.sh`.

## UI generation constraints

AI-generated UI and chat output must follow:

- [docs/ui/UI_DESIGN_SYSTEM.md](docs/ui/UI_DESIGN_SYSTEM.md) — theming, spacing, copy rules (e.g. confirm-leading actions end with `…`)
- [docs/ui/RML_PROFILE.md](docs/ui/RML_PROFILE.md) — allowed RML elements, structured JSON chat blocks
- [docs/ui/RCSS_PROFILE.md](docs/ui/RCSS_PROFILE.md) — supported RCSS properties

Prompt text for LLMs is built in [`src/base/ai/PromptBuilder.cpp`](src/base/ai/PromptBuilder.cpp).

## Common tasks

| Task | Where to look |
|------|----------------|
| Default chat UI | `assets/samples/window_shell.rml`, `assets/views/home.rml`, `assets/views/chat.rml`, `src/feature/chat/ChatController.cpp` |
| Window shell / layout | `src/feature/ui/ShellHost.*`, [docs/ui/WINDOW_SHELL.md](docs/ui/WINDOW_SHELL.md) |
| Working set panel | [docs/ui/WORKING_SET_PANEL.md](docs/ui/WORKING_SET_PANEL.md) — auxiliary pane design |
| Theme / layout | `assets/themes/base.rcss` |
| App entry / chat bootstrap | `src/app/Application.cpp`, `src/app/main.cpp`, `src/feature/chat/ChatController.cpp` |
| Structured AI replies | `src/base/ai/StructuredTextParser.cpp` |
| Turn planning pipeline | `src/base/ai/TurnPlan.*`, `src/feature/ai/PayloadTurnPlanBuilder.*`, `TurnPlanner.*`, `TurnExecutor.*`, `AgentSession.cpp` |
| AI-centric intent / agency (long-term) | [projects/ai-centric-interface/](projects/ai-centric-interface/) — 10 acts, open domains; v1 thin coverage first |
| P2P messaging | `src/feature/messaging/`, [docs/architecture/P2P_MESSAGING.md](docs/architecture/P2P_MESSAGING.md), [docs/contracts/WIRE_SCHEMAS.md](docs/contracts/WIRE_SCHEMAS.md) |
| Libp2p stream framing / hangs | [docs/architecture/LIBP2P_STREAMS.md](docs/architecture/LIBP2P_STREAMS.md), `src/base/p2p/StreamFrameIo.*` |
| P2P mesh | [projects/p2p-mesh/](projects/p2p-mesh/) — **nf** + **n4-media** done; **N023** relay scope ([RELAY_SCOPE.md](projects/p2p-mesh/RELAY_SCOPE.md)); **N022** invest libp2p; **N026** media-relay attach SM design ([MEDIA_RELAY_ATTACH.md](projects/p2p-mesh/MEDIA_RELAY_ATTACH.md)) |
| P2P A/V calls | [projects/p2p-av-calls/](projects/p2p-av-calls/) — **V026** libp2p media (**m1** mobile LAN OK; **m2** teardown done); **V033** session SMs + circuit compose; **code map** [docs/architecture/CALLS.md](docs/architecture/CALLS.md) |
| Media hop reachability | [projects/media-hop-reachability/](projects/media-hop-reachability/) — **in-libp2p** (L0 docs; L1 next) |
| Network status chrome | [projects/network-status-chrome/](projects/network-status-chrome/) — **s3 landed**; s4 polish next — [DESIGN](projects/network-status-chrome/DESIGN.md) |
| Contacts UI / store | `src/feature/ui/ContactsController.*`, `src/base/people/ContactsStore.*`, `assets/views/contacts.rml`, `contact_detail.rml` |
| Profile icons / chat attachments | [projects/relay-blob-upload/](projects/relay-blob-upload/) — **a1–a6 + a5 done** — Smart policy, suppression, peer chat-blob, fetch ladder, outbound peer upload, DEK-wrap, video poster |
| SQLite thread store | `src/base/messaging/SqliteThreadStore.*`, `ChatPayloadCodec.*` — [projects/chat-storage-and-memory/](projects/chat-storage-and-memory/) |
| E2E symmetric crypto (`base/crypto`) | `src/base/crypto/`, [docs/contracts/MESSAGE_ENCRYPTION.md](docs/contracts/MESSAGE_ENCRYPTION.md) — [projects/e2e-message-crypto/](projects/e2e-message-crypto/); PQ natives + libsodium via [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto) |
| At-rest encryption (PIN vault) | `ProfileSecretsService`, `DataKeyVault`, `IDekConsumer`, `PinGateController`, [docs/contracts/AT_REST_ENCRYPTION.md](docs/contracts/AT_REST_ENCRYPTION.md) — [projects/at-rest-crypto/](projects/at-rest-crypto/) |
| Multi-device / Account ID | [projects/multi-device-account/](projects/multi-device-account/) — **m3 `endpoints[]` landed**; next **m4c** paste contacts (M018) — amends D096 (D099), E025, A010 |
| PIN chooser / Change PIN | `PinGateController`, `SecuritySettingsSection`, Me → Security — ADR A007 in [projects/at-rest-crypto/DECISIONS.md](projects/at-rest-crypto/DECISIONS.md) |
| Chat storage / memory | [projects/chat-storage-and-memory/](projects/chat-storage-and-memory/) — **Waves 1–2 done**; Wave 3 next (v3 ∥ v4) |
| Config / data / profiles | `src/app/Bootstrap.*`, `src/base/data/`, `src/base/runtime/`, `src/base/platform/`, [docs/contracts/DATA_LAYOUT.md](docs/contracts/DATA_LAYOUT.md), [docs/ops/CONFIGURATION.md](docs/ops/CONFIGURATION.md), [docs/contracts/COMPATIBILITY.md](docs/contracts/COMPATIBILITY.md) |
| Doc map / contracts | [docs/README.md](docs/README.md) |
| In-app settings (Me tab) | `src/feature/ui/SettingsController.*`, `assets/views/settings.rml` |
| Threading / async | [docs/architecture/THREADING.md](docs/architecture/THREADING.md) — `AppRuntime`, coordinator, worker pool |
| Build | [docs/ops/BUILD.md](docs/ops/BUILD.md) |
| Writing unit tests | [docs/ops/TEST_STRATEGY.md](docs/ops/TEST_STRATEGY.md#unit-test-conventions) — temp SQLite dirs, Windows file locks, gtest fixtures |
| macOS signing / notarization | [docs/ops/MACOS_SIGNING.md](docs/ops/MACOS_SIGNING.md) |
| Source layers | [docs/architecture/SRC_LAYOUT.md](docs/architecture/SRC_LAYOUT.md) |
| UI vs functional decoupling | [docs/architecture/UI_FUNCTIONAL_BOUNDARY.md](docs/architecture/UI_FUNCTIONAL_BOUNDARY.md), [RUNTIME_COMPOSITION.md](docs/architecture/RUNTIME_COMPOSITION.md) |

## Conventions

- Prefer extending existing patterns (`ChatController`, `DataModelHost`, `SettingsCommands` ports) over new frameworks.
- Do **not** add new `::Instance()` coupling between UI controllers or functional → `ShellHost::State()` writes — see [UI_FUNCTIONAL_BOUNDARY.md](docs/architecture/UI_FUNCTIONAL_BOUNDARY.md).
- Avoid unsupported RCSS (see RCSS profile); RmlUi will log parse errors at runtime.
- For chat bubbles, use `selectable="text"` and `focus: none` so the draft textarea keeps focus. Suggestion buttons render inline inside assistant bubbles.
- Keep fork diffs focused; note them in `RMLUI_UPSTREAM.md` when adding capabilities.
- Respect layer dependencies: `app → feature → base → common` (see [SRC_LAYOUT.md](docs/architecture/SRC_LAYOUT.md)).
- Prefer `#include` over forward declarations when the type is already a legal dependency (lower layer or allowed feature edge). Use forward decls to break cycles / upward edges, not to “lean” headers past `base`/`common` types — details in [SRC_LAYOUT.md](docs/architecture/SRC_LAYOUT.md#prefer-include-over-forward-declaration).
- **Temp SQLite dirs in tests:** never call `std::filesystem::remove_all` while `SqliteThreadStore` (or any object holding an open `sqlite3*`) is still alive — Windows CI fails with *file in use*. Use a gtest fixture; hold stores in `std::unique_ptr`; `reset()` them in `TearDown()` before cleanup. See [TEST_STRATEGY.md § Unit test conventions](docs/ops/TEST_STRATEGY.md#unit-test-conventions).
