# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** **top-level `feature/calls` landed** → gui bands / hub CallStack ownership later

## Shipped this stream

- f1–f3 peels; F004/F006/F007 naming
- f4v1 nested calls → **lifted to `feature/calls/` + `pp_feature_calls`**
- f5v1 / f7v1: `src/gui/`
- f6 soft edge: `AgentInboundPorts`
- F007 rename: `feature/conversations`
- Call soft edges: `CallDeliveryPorts` + the `call` system-control family (replaced `CallControlInboundPorts`)

## Paths today

| Path | Role |
|------|------|
| `feature/conversations/` | Conversations hub + delivery |
| `feature/calls/` | Call session (`pp_feature_calls`) |
| `gui/` | Product presenters |
| `lib/messaging/` | Record/codec engines (unchanged) |

Link order: `conversations → calls`; app fills AI inbound ports; hub fills call delivery/inbound ports.

## Next

1. Optional: move `CallStack` ownership from hub to app.
2. Optional `gui/` band nesting (`call/`, `settings/`, `shared/`).
3. Inbox presentation extraction.
4. Optional rename residual `Messaging*Ports` / `MeshDeliveryOrchestrator` names.

## Source layout migration (history, from SRC_LAYOUT.md)

1. Enforce lib peer bans in CI (`check_base_includes.sh` + `check_base_public_libs.sh` + legacy allowlists) for **new** edges. **Started:** peels into `common/{directory,thread,chat,media,ui}/` with thin thread headers + role ports (`IThreadCatalog` / `Transcript` / `Memory` / `Sync`); attachment upload/fetch helpers live in `feature/conversations`. Removed legacy edges include `mesh→people/net/media`, `ai→ui/messaging/net`, `messaging→net`, `ai→net`.
2. Extract remaining cross-peer types/helpers (`net`↔messaging/people stores and relay sign helpers).
3. Move foundation folders to `src/foundation/`; update includes/CMake. **Done.**
4. Move the peer folders out of the aggregate “base” folder. **Done**; renamed `src/domain/` → `src/lib/` (`pp_lib`, 2026-10-23) so that `domain` can name the cross-lib tier.
5. Feature/app/gui cleanup. **In progress:** [`projects/feature-layer-reorg/`](../../projects/feature-layer-reorg/) — f7v1 shipped `src/gui/` ([F008](../../projects/feature-layer-reorg/DECISIONS.md#f008--gui-layer-above-feature)); f6 soft edge + `feature/conversations` rename + top-level `feature/calls` / `pp_feature_calls` ([F004](../../projects/feature-layer-reorg/DECISIONS.md#f004--calls-home-nested-band-first-then-top-level)) shipped; `CallStack` / `BroadcastHub` moved out of the hub to `feature/product` (participants); remaining: optional gui bands, inbox presentation.
6. **Domain tier (2026-10-23).** Added `src/domain/` between feature and lib for capabilities that features share. First tenants: `registration`, `node`, `call_media` (out of feature/) and `peers`, `attachments` (out of feature/conversations). Next: split the delivery core out of `MeshDeliveryOrchestrator`; it holds the chat sync, inbox and group membership, so this is a redesign, not a move.
