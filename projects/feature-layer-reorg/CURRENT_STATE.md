# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** **f5v1 bands landed** (chat absorbed; shell/contacts nested under ui) → f6 wirers / top-level splits later

## Shipped this stream

- f1–f3 peels; F004/F006/F007 naming
- f4v1: `feature/messaging/calls/`
- **f5v1:** retired top-level `feature/chat` → `feature/ui/chat/`; nested `feature/ui/shell/` + `feature/ui/contacts/`; single `pp_feature_ui` (now links `pp_feature_ai`)

## Paths today

| Path | Role |
|------|------|
| `feature/messaging/` | Conversations hub + delivery (legacy name) |
| `feature/messaging/calls/` | Call session |
| `feature/ui/` | Residual presenters (settings, call UI, pin, emoji, …) |
| `feature/ui/shell/` | ShellHost, mount, shell ports |
| `feature/ui/contacts/` | Contacts + people-picker |
| `feature/ui/chat/` | ChatController + screen helpers |

Link order: `settings → ai → messaging → ui` (no `pp_feature_chat`).

## Next

1. **f6:** named Application wirers; optional conversations→ai inbound port.
2. Later: top-level `feature/calls` / `feature/shell` / `feature/contacts` / rename messaging→conversations when cycles allow.
3. Promote SRC_LAYOUT when stable (partially updated).
