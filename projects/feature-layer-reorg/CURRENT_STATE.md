# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** **top-level `feature/calls` landed** → gui bands / hub CallStack ownership later

## Shipped this stream

- f1–f3 peels; F004/F006/F007 naming
- f4v1 nested calls → **lifted to `feature/calls/` + `pp_feature_calls`**
- f5v1 / f7v1: `src/gui/`
- f6 soft edge: `AgentInboundPorts`
- F007 rename: `feature/conversations`
- Call soft edges: `CallDeliveryPorts` + `CallControlInboundPorts`

## Paths today

| Path | Role |
|------|------|
| `feature/conversations/` | Conversations hub + delivery |
| `feature/calls/` | Call session (`pp_feature_calls`) |
| `gui/` | Product presenters |
| `domain/messaging/` | Record/codec engines (unchanged) |

Link order: `conversations → calls`; app fills AI inbound ports; hub fills call delivery/inbound ports.

## Next

1. Optional: move `CallStack` ownership from hub to app.
2. Optional `gui/` band nesting (`call/`, `settings/`, `shared/`).
3. Inbox presentation extraction.
4. Optional rename residual `Messaging*Ports` / `MeshDeliveryOrchestrator` names.
