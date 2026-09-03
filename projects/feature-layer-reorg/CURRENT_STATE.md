# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** **f6 soft edge landed** (`AgentInboundPorts`) → calls rename / gui bands later

## Shipped this stream

- f1–f3 peels; F004/F006/F007 naming
- f4v1: `feature/messaging/calls/`
- f5v1: nested ui bands (staging)
- f6v1: named Application wirers
- **F008 / f7v1:** lifted `feature/ui/**` → `src/gui/**`; `pp_gui`; retired `pp_feature_ui`
- **f6 soft edge:** conversations invoke AI via `AgentInboundPorts`; `AgentUiPorts` lives in `feature/ai/`

## Paths today

| Path | Role |
|------|------|
| `feature/messaging/` | Conversations hub + delivery (legacy name) |
| `feature/messaging/calls/` | Call session |
| `gui/` | Product presenters (settings, call, pin, emoji, …) |
| `gui/shell/` | ShellHost, mount, shell ports |
| `gui/contacts/` | Contacts + people-picker |
| `gui/chat/` | ChatController + screen helpers |
| `domain/ui/` | Non-Rml presentation policy (not the GUI layer) |

Link order: `app → gui → feature`. Feature modules do not link each other for AI; app fills `AgentInboundPorts`.

## Next

1. Later: top-level `feature/calls` / rename messaging→conversations when cycles allow.
2. Optional `gui/` band nesting (`call/`, `settings/`, `shared/`).
3. Inbox presentation extraction (highest product risk).
