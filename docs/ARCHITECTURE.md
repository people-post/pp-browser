# Architecture

pp-browser is a native AI-oriented UI shell:

- **SDL3 + OpenGL3** — windowing and GPU presentation
- **Hard-forked RmlUi** — RML/RCSS layout and widgets (`src/render/fork/`)
- **Hard-forked libp2p** — C++ libp2p stack (`src/libp2p/fork/`)
- **ActionRouter** — maps UI events to MCP tool calls via bindings manifest
- **UiGenerator** — LLM produces RML/RCSS/bindings from MCP tool schemas

Source code follows a four-layer layout — see [SRC_LAYOUT.md](SRC_LAYOUT.md).

```
Application → Backend (SDL_GL3) → RmlUi Context
           → DocumentLoader / Theme / DataModelHost
           → ActionRouter → McpClient
           → UiGenerator → LlmClient
```

Presentation (RML/RCSS) is separate from behavior (C++ action routing and MCP).

## Chat agent

Multi-turn chat uses a shared [`Conversation`](../src/base/ai/conversation/Conversation.h) transcript for UI and LLM context. See [AGENT_CONVERSATION.md](AGENT_CONVERSATION.md).

## Dynamic RML (`RmlMount`)

Runtime UI updates use [`src/feature/ui/RmlMount.cpp`](../src/feature/ui/RmlMount.cpp):

- `MountInner` — browser-like inner markup mount (`SetInnerRML`) with optional validation, focus, and scroll preservation (`data-mount-id`)
- `InjectRcss` — merge dynamic RCSS into the active document (re-inject by `source_tag` replaces prior rules)
- `DocumentLoader::MountFragment` — mount into a live container without closing the active document

Demo: `./pp-browser --demo dynamic`

## Window Shell

Chat and future full-window demos use the role-based shell in [`src/feature/ui/ShellHost.*`](../src/feature/ui/ShellHost.cpp):

- **ShellLayout** — Compact/Expanded modes (768dp breakpoint)
- **ShellHost** — Primary/Secondary/Auxiliary panes, overlays, Safari-style compact toolbar
- **ShellInterruption** — Escape dismiss ordering
- **ShellFeedback** — Banner, toast, alert/confirm dialog

See [WINDOW_SHELL.md](WINDOW_SHELL.md).
