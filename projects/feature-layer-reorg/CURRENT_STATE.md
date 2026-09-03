# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** **f6v1 done** → **f7 next** (`feature/ui/**` → `src/gui/` per [F008](DECISIONS.md#f008--gui-layer-above-feature))

## Shipped this stream

- f1–f3 peels; F004/F006/F007 naming
- f4v1: `feature/messaging/calls/`
- **f5v1:** retired top-level `feature/chat` → `feature/ui/chat/`; nested `feature/ui/shell/` + `feature/ui/contacts/`; single `pp_feature_ui` (staging for gui)
- **f6v1:** named `Application` wirers
- **F008:** end-state product UI layer named **`gui`** (above feature); `domain/ui` remains policy

## Paths today

| Path | Role |
|------|------|
| `feature/messaging/` | Conversations hub + delivery (legacy name) |
| `feature/messaging/calls/` | Call session |
| `feature/ui/` | **Staging** for `src/gui/` — residual presenters (settings, call, pin, emoji, …) |
| `feature/ui/shell/` | ShellHost, mount, shell ports |
| `feature/ui/contacts/` | Contacts + people-picker |
| `feature/ui/chat/` | ChatController + screen helpers |
| `domain/ui/` | Non-Rml presentation policy (not the GUI layer) |

Link order today: `settings → ai → messaging → ui` (no `pp_feature_chat`).  
Target: `app → gui → feature → …` ([NORTH_STAR.md](NORTH_STAR.md)).

## Next

1. **f7:** lift `feature/ui/**` → `src/gui/**`; retire `pp_feature_ui`; ban `feature → gui`.
2. Optional f6 soft edge: conversations→ai inbound port.
3. Later: top-level `feature/calls` / rename messaging→conversations when cycles allow (separate from f7).
4. Promote SRC_LAYOUT when `gui/` path ships.
