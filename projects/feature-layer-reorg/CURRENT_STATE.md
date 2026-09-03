# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** f3 landed → **f4v1 nested calls band next**

## What already shipped (prior work)

- `src/base/` retired → `src/foundation/` + `src/domain/`
- Domain peers strictly independent; cross-need via `common/` + feature wiring
- Feature module libs acyclic: `settings → ai → messaging → ui → chat`
- Ports + app bridges break UI↔functional cycles ([UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md))
- Include guard: [`scripts/check_feature_includes.sh`](../../scripts/check_feature_includes.sh)
- **f0** project docs + **F006** (sure peels use existing peers, no new top-level domain peers)
- **f1+f2** sure peels into `domain/{messaging,people,mesh/reachability,ui}` (Hub still owns `unique_ptr`s)
- **f3** `IDirectMessageClient` → `common/chat/`; `ChatHistoryResponder` → `domain/messaging` with sign callback

## Snapshot (approx.)

| Area | Notes |
|------|-------|
| `feature/messaging` | Hub/sync/calls/Amp adapters; stores/PSK/reach/history responder peeled |
| `feature/ui` | Uses `domain/people` for reachability |
| `domain/messaging` | + PSK/call-key/epoch + `ChatHistoryResponder` |
| `common/chat` | + `IDirectMessageClient` |

## Pain points (remaining)

1. Calls still under `feature/messaging` — **f4v1 nested band** (avoid `pp_feature_calls` cycle with MeshMessagingService).
2. `feature/ui` grab-bag — f5.
3. Inbox presentation / ChatController — f6.
4. messaging → ai hard-includes `AgentSession`.
5. Cross-peer utils still blocked.

## Next agent — start here

1. **f4v1:** ADR F004 = nest `feature/messaging/calls/` (same CMake target); move Call* files; no new lib yet.
2. Do not add top-level `pp_feature_calls` until MeshMessagingService edge is ported.
3. Optional parallel: app named wirers.

## Related docs still slightly stale

| Doc | Stale bit |
|-----|-----------|
| `RUNTIME_COMPOSITION.md` | Diagrams may still say `base/` |

Fix those opportunistically when touching layout.
