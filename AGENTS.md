# Agent guide for pp-browser

This document orients coding agents working in this repository.

## Architecture

pp-browser is a native AI-oriented UI shell:

- **SDL3 + OpenGL3** — windowing (`src/render/integration/`)
- **Hard-forked RmlUi** — UI layout in `src/render/fork/`
- **Hard-forked libp2p** — P2P networking in `src/libp2p/`
- **Third-party libs** — FreeType, nlohmann/json, curl, SDL3, SDL3_image, and libp2p deps in [`third_party/`](third_party/)
- **Four-layer source tree** — `src/common/`, `src/base/`, `src/feature/`, `src/app/` — see [docs/SRC_LAYOUT.md](docs/SRC_LAYOUT.md)

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full picture.

## RmlUi is maintained in-tree

We **own and modify** the vendored copy under [`src/render/fork/`](src/render/fork/). It is not a submodule.

- Edit RmlUi directly when app-level workarounds are insufficient (layout, text selection, new properties, etc.).
- Document fork-specific changes in [docs/RMLUI_UPSTREAM.md](docs/RMLUI_UPSTREAM.md).
- App-specific SDL/GL glue stays in [`src/render/integration/`](src/render/integration/), not in the fork.

### Fork features (pp-browser)

| Feature | Location | Usage |
|---------|----------|--------|
| Text selection in static content | `src/render/fork/Source/Core/Elements/ElementSelectableText.*`, `SelectionController.*` | RML attribute `selectable="text"`; participation API on `Element`; Ctrl+C copies selection |
| User-agent baseline styles | `src/render/fork/Source/Core/UserAgentStyleSheet.*` | Auto-merged into every document; author RCSS overrides |
| List markers (workaround) | `src/render/fork/Source/Core/ListMarker.*`, `Layout/InlineLevelBox.cpp` | `ul`/`ol` bullets until `list-style` exists — see [RMLUI_UPSTREAM.md](docs/RMLUI_UPSTREAM.md) |

## libp2p is maintained in-tree

We **own and modify** the hard fork under [`src/libp2p/`](src/libp2p/). It is not a submodule. Hunter is removed; dependencies are vendored in `third_party/`.

- Edit libp2p directly when protocol or transport changes are needed.
- Document fork-specific changes in [docs/LIBP2P_UPSTREAM.md](docs/LIBP2P_UPSTREAM.md).
- App-specific glue will live in [`src/libp2p/integration/`](src/libp2p/integration/) (not in the fork proper).
- Import/update libp2p deps with `./scripts/libp2p_vendor_import.sh`.

## UI generation constraints

AI-generated UI and chat output must follow:

- [docs/RML_PROFILE.md](docs/RML_PROFILE.md) — allowed RML elements, structured JSON chat blocks
- [docs/RCSS_PROFILE.md](docs/RCSS_PROFILE.md) — supported RCSS properties

Prompt text for LLMs is built in [`src/base/ai/PromptBuilder.cpp`](src/base/ai/PromptBuilder.cpp).

## Common tasks

| Task | Where to look |
|------|----------------|
| Default chat UI | `assets/samples/window_shell.rml`, `assets/views/chat.rml`, `src/feature/chat/ChatDemo.cpp` |
| Window shell / layout | `src/feature/ui/ShellHost.*`, [docs/WINDOW_SHELL.md](docs/WINDOW_SHELL.md) |
| Working set panel | [docs/WORKING_SET_PANEL.md](docs/WORKING_SET_PANEL.md) — auxiliary pane design |
| Theme / layout | `assets/themes/base.rcss` |
| Wire new demo | `src/app/Application.cpp`, `src/app/main.cpp` (`--demo`) |
| Structured AI replies | `src/base/ai/StructuredTextParser.cpp` |
| Turn planning pipeline | `src/base/ai/TurnPlan.*`, `src/feature/ai/PayloadTurnPlanBuilder.*`, `TurnPlanner.*`, `TurnExecutor.*`, `AgentSession.cpp` |
| P2P messaging | `src/feature/messaging/`, [docs/P2P_MESSAGING.md](docs/P2P_MESSAGING.md) |
| Config / data / profiles | `src/app/Bootstrap.*`, `src/base/data/`, `src/base/platform/`, [docs/CONFIGURATION.md](docs/CONFIGURATION.md) |
| In-app settings | `src/feature/ui/SettingsController.*`, `assets/views/settings.rml` |
| Build | [docs/BUILD.md](docs/BUILD.md) |
| Source layers | [docs/SRC_LAYOUT.md](docs/SRC_LAYOUT.md) |

## Conventions

- Prefer extending existing patterns (`SearchDemo`, `ChatDemo`, `DataModelHost`) over new frameworks.
- Avoid unsupported RCSS (see RCSS profile); RmlUi will log parse errors at runtime.
- For chat bubbles, use `selectable="text"` and `focus: none` so the draft textarea keeps focus. Suggestion buttons render inline inside assistant bubbles.
- Keep fork diffs focused; note them in `RMLUI_UPSTREAM.md` when adding capabilities.
- Respect layer dependencies: `app → feature → base → common` (see [SRC_LAYOUT.md](docs/SRC_LAYOUT.md)).
