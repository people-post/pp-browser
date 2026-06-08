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
