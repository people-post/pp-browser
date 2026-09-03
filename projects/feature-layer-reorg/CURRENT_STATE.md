# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** **f4v1 nested calls band landed** → f5 (shell/contacts + absorb chat) or top-level calls when unblocked

## What already shipped

- Foundation/domain split; f1–f3 sure peels; **F004/F006/F007** naming
- **f4v1:** `feature/messaging/calls/` band (same `pp_feature_messaging`) — CallStack, CSM, lifecycle, topology, CallUiBackend, Amp circuit/media façades

## Locked naming ([F007](DECISIONS.md#f007--vocabulary--end-state-feature-names))

| End-state folder | Today | Role |
|------------------|-------|------|
| `conversations/` | `feature/messaging` (parent) | Conversations hub + delivery |
| `calls/` | `feature/messaging/calls/` | Call **session** (nested; top-level later) |
| `ui/` (+ shell/contacts) | `feature/ui` + `feature/chat` | Presenters; no top-level chat/ |
| `domain/messaging` | same | Record/codec engines |

## Snapshot

| Area | Notes |
|------|-------|
| `feature/messaging/` | Conversations hub + delivery; Call\* moved under `calls/` |
| `feature/messaging/calls/` | Call session orchestration (f4v1) |
| `feature/chat` | Still top-level — absorb in f5 |
| `feature/ui` | Grab-bag + `CallController` |

## Next agent — start here

1. **f5:** split shell/contacts from ui; move `ChatController` into ui/shell; retire top-level `feature/chat`.
2. Or port MeshMessagingService edge and lift `calls/` to top-level `pp_feature_calls`.
3. Optional: app named wirers (`WireConversations` / `WireCalls`).

Do **not** invent `domain/calls`. Do **not** reintroduce top-level `feature/chat` in the plan.
