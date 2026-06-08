# Agent guide for pp-browser

This document orients coding agents working in this repository.

## Architecture

pp-browser is a native AI-oriented UI shell:

- **SDL3 + OpenGL3** — windowing (`src/render/backends/`)
- **Hard-forked RmlUi** — UI layout in `thirdparty/rmlui/`
- **Chat + MCP + LLM scaffolding** — `src/demo/`, `src/agent/`, `src/mcp/`

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full picture.

## RmlUi is maintained in-tree

We **own and modify** the vendored copy under [`thirdparty/rmlui/`](thirdparty/rmlui/). It is not a submodule.

- Edit RmlUi directly when app-level workarounds are insufficient (layout, text selection, new properties, etc.).
- Document fork-specific changes in [docs/RMLUI_UPSTREAM.md](docs/RMLUI_UPSTREAM.md).
- Re-sync from upstream with `./scripts/import-rmlui.sh <tag>`, then re-apply pp-browser patches.
- App-specific SDL/GL glue stays in [`src/render/backends/`](src/render/backends/), not in the fork.

### Fork features (pp-browser)

| Feature | Location | Usage |
|---------|----------|--------|
| Text selection in static content | `thirdparty/rmlui/Source/Core/TextSelectionController.*` | RML attribute `selectable="text"`; Ctrl+C copies selection |

## UI generation constraints

AI-generated UI and chat output must follow:

- [docs/RML_PROFILE.md](docs/RML_PROFILE.md) — allowed RML elements, structured JSON chat blocks
- [docs/RCSS_PROFILE.md](docs/RCSS_PROFILE.md) — supported RCSS properties

Prompt text for LLMs is built in [`src/agent/PromptBuilder.cpp`](src/agent/PromptBuilder.cpp).

## Common tasks

| Task | Where to look |
|------|----------------|
| Default chat UI | `assets/samples/chat_dialog.rml`, `src/demo/ChatDemo.cpp` |
| Theme / layout | `assets/themes/base.rcss` |
| Wire new demo | `src/app/Application.cpp`, `src/main.cpp` (`--demo`) |
| Structured AI replies | `src/agent/StructuredTextParser.cpp` |
| Build | [docs/BUILD.md](docs/BUILD.md) |

## Conventions

- Prefer extending existing patterns (`SearchDemo`, `ChatDemo`, `DataModelHost`) over new frameworks.
- Avoid unsupported RCSS (see RCSS profile); RmlUi will log parse errors at runtime.
- For chat bubbles, use `selectable="text"` and `focus: none` so the draft textarea keeps focus.
- Keep fork diffs focused; note them in `RMLUI_UPSTREAM.md` when adding capabilities.
