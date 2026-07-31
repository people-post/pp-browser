# Agent guide for pp-browser

This document orients coding agents working in this repository.

## Architecture

pp-browser is a native AI-oriented UI shell:

- **SDL3 + OpenGL3** — windowing (`src/render/integration/`)
- **Hard-forked RmlUi** — UI layout in `src/render/fork/`
- **Hard-forked libp2p** — P2P networking in `src/libp2p/fork/`
- **Third-party libs** — FreeType, nlohmann/json, curl, SDL3, SDL3_image, and libp2p deps in [`third_party/`](third_party/)
- **Four-layer source tree** — `src/common/`, `src/base/`, `src/feature/`, `src/app/` — see [docs/architecture/SRC_LAYOUT.md](docs/architecture/SRC_LAYOUT.md)

See [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) for the full picture. Doc tiers (architecture / contracts / ops): [docs/README.md](docs/README.md). Compatibility (dirty disk, newer peers): [docs/contracts/COMPATIBILITY.md](docs/contracts/COMPATIBILITY.md).

## RmlUi is maintained in-tree

We **own and modify** the vendored copy under [`src/render/fork/`](src/render/fork/). It is not a submodule.

- Edit RmlUi directly when app-level workarounds are insufficient (layout, text selection, new properties, etc.).
- Document fork-specific changes in [docs/architecture/RMLUI_UPSTREAM.md](docs/architecture/RMLUI_UPSTREAM.md).
- App-specific SDL/GL glue stays in [`src/render/integration/`](src/render/integration/), not in the fork.

### Fork features (pp-browser)

| Feature | Location | Usage |
|---------|----------|--------|
| Text selection in static content | `src/render/fork/Source/Core/Elements/ElementSelectableText.*`, `SelectionController.*` | RML attribute `selectable="text"`; participation API on `Element`; Ctrl+C copies selection |
| User-agent baseline styles | `src/render/fork/Source/Core/UserAgentStyleSheet.*` | Auto-merged into every document; author RCSS overrides |
| List markers (workaround) | `src/render/fork/Source/Core/ListMarker.*`, `Layout/InlineLevelBox.cpp` | `ul`/`ol` bullets until `list-style` exists — see [RMLUI_UPSTREAM.md](docs/architecture/RMLUI_UPSTREAM.md) |

## libp2p is maintained in-tree

We **own and modify** the hard fork under [`src/libp2p/fork/`](src/libp2p/fork/). It is not a submodule. Hunter is removed; dependencies are vendored in `third_party/`.

- Edit libp2p directly when protocol or transport changes are needed.
- Document fork-specific changes in [docs/architecture/LIBP2P_UPSTREAM.md](docs/architecture/LIBP2P_UPSTREAM.md).
- App-specific glue lives in [`src/libp2p/integration/host/`](src/libp2p/integration/host/) (not in the fork proper).
- Import/update libp2p deps with `./scripts/libp2p_vendor_import.sh`.

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
| P2P mesh | [projects/p2p-mesh/](projects/p2p-mesh/) — **nf** + **n4-media** done |
| P2P A/V calls | [projects/p2p-av-calls/](projects/p2p-av-calls/) — **a4 thin** landed (SFU soft-migrate + V024); polish next |
| Contacts UI / store | `src/feature/ui/ContactsController.*`, `src/base/people/ContactsStore.*`, `assets/views/contacts.rml`, `contact_detail.rml` |
| SQLite thread store | `src/base/messaging/SqliteThreadStore.*`, `ChatPayloadCodec.*` — [projects/chat-storage-and-memory/](projects/chat-storage-and-memory/) |
| E2E symmetric crypto (`base/crypto`) | `src/base/crypto/`, [docs/contracts/MESSAGE_ENCRYPTION.md](docs/contracts/MESSAGE_ENCRYPTION.md) — [projects/e2e-message-crypto/](projects/e2e-message-crypto/) |
| At-rest encryption (PIN vault) | `ProfileSecretsService`, `DataKeyVault`, `IDekConsumer`, `PinGateController`, [docs/contracts/AT_REST_ENCRYPTION.md](docs/contracts/AT_REST_ENCRYPTION.md) — [projects/at-rest-crypto/](projects/at-rest-crypto/) |
| PIN chooser / Change PIN | `PinGateController`, `SecuritySettingsSection`, Me → Security — ADR A007 in [projects/at-rest-crypto/DECISIONS.md](projects/at-rest-crypto/DECISIONS.md) |
| Chat storage / memory | [projects/chat-storage-and-memory/](projects/chat-storage-and-memory/) — **Waves 1–2 done**; Wave 3 next (v3 ∥ v4) |
| Config / data / profiles | `src/app/Bootstrap.*`, `src/base/data/`, `src/base/platform/`, [docs/contracts/DATA_LAYOUT.md](docs/contracts/DATA_LAYOUT.md), [docs/ops/CONFIGURATION.md](docs/ops/CONFIGURATION.md), [docs/contracts/COMPATIBILITY.md](docs/contracts/COMPATIBILITY.md) |
| Doc map / contracts | [docs/README.md](docs/README.md) |
| In-app settings (Me tab) | `src/feature/ui/SettingsController.*`, `assets/views/settings.rml` |
| Build | [docs/ops/BUILD.md](docs/ops/BUILD.md) |
| macOS signing / notarization | [docs/ops/MACOS_SIGNING.md](docs/ops/MACOS_SIGNING.md) |
| Source layers | [docs/architecture/SRC_LAYOUT.md](docs/architecture/SRC_LAYOUT.md) |

## Conventions

- Prefer extending existing patterns (`ChatController`, `DataModelHost`) over new frameworks.
- Avoid unsupported RCSS (see RCSS profile); RmlUi will log parse errors at runtime.
- For chat bubbles, use `selectable="text"` and `focus: none` so the draft textarea keeps focus. Suggestion buttons render inline inside assistant bubbles.
- Keep fork diffs focused; note them in `RMLUI_UPSTREAM.md` when adding capabilities.
- Respect layer dependencies: `app → feature → base → common` (see [SRC_LAYOUT.md](docs/architecture/SRC_LAYOUT.md)).
- Prefer `#include` over forward declarations when the type is already a legal dependency (lower layer or allowed feature edge). Use forward decls to break cycles / upward edges, not to “lean” headers past `base`/`common` types — details in [SRC_LAYOUT.md](docs/architecture/SRC_LAYOUT.md#prefer-include-over-forward-declaration).
