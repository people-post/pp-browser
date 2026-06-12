# Architecture

pp-browser is a native AI-oriented UI shell:

- **SDL3 + OpenGL3** — windowing and GPU presentation
- **Hard-forked RmlUi** — RML/RCSS layout and widgets (`src/render/`)
- **ActionRouter** — maps UI events to MCP tool calls via bindings manifest
- **UiGenerator** — LLM produces RML/RCSS/bindings from MCP tool schemas

```
Application → Backend (SDL_GL3) → RmlUi Context
           → DocumentLoader / Theme / DataModelHost
           → ActionRouter → McpClient
           → UiGenerator → LlmClient
```

Presentation (RML/RCSS) is separate from behavior (C++ action routing and MCP).

## Dynamic RML (`RmlMount`)

Runtime UI updates use [`src/ui/RmlMount.cpp`](../src/ui/RmlMount.cpp):

- `MountInner` — browser-like inner markup mount (`SetInnerRML`) with optional validation, focus, and scroll preservation (`data-mount-id`)
- `InjectRcss` — merge dynamic RCSS into the active document (re-inject by `source_tag` replaces prior rules)
- `DocumentLoader::MountFragment` — mount into a live container without closing the active document

Demo: `./pp-browser --demo dynamic`
