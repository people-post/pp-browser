# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** **conversations rename landed** → top-level `feature/calls` / gui bands later

## Shipped this stream

- f1–f3 peels; F004/F006/F007 naming
- f4v1: `feature/conversations/calls/` (nested)
- f5v1: nested ui bands (staging)
- f6v1: named Application wirers
- **F008 / f7v1:** lifted `feature/ui/**` → `src/gui/**`; `pp_gui`; retired `pp_feature_ui`
- **f6 soft edge:** conversations invoke AI via `AgentInboundPorts`; `AgentUiPorts` lives in `feature/ai/`
- **F007 rename:** `feature/messaging` → `feature/conversations`; `ConversationsHub` / `ConversationsFacade`; `pp_feature_conversations`

## Paths today

| Path | Role |
|------|------|
| `feature/conversations/` | Conversations hub + delivery |
| `feature/conversations/calls/` | Call session (still nested; top-level deferred) |
| `gui/` | Product presenters (settings, call, pin, emoji, …) |
| `gui/shell/` | ShellHost, mount, shell ports |
| `gui/contacts/` | Contacts + people-picker |
| `gui/chat/` | ChatController + screen helpers |
| `domain/ui/` | Non-Rml presentation policy (not the GUI layer) |
| `domain/messaging/` | Record/codec engines (unchanged peer name) |

Link order: `app → gui → feature`. Feature modules do not link each other for AI; app fills `AgentInboundPorts`.

## Next

1. Top-level `feature/calls` + `pp_feature_calls` after delivery ports break Hub↔CSM cycle.
2. Optional `gui/` band nesting (`call/`, `settings/`, `shared/`).
3. Inbox presentation extraction (highest product risk).
4. Optional rename residual `Messaging*Ports` / `MeshMessagingService` names.
