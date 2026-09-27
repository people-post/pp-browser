# Thread ownership — current state

**Last updated:** 2026-09-27
**Branch:** `refactor/peer-reach-coordinator` (after media-client-layers l8)

## Landed

| Phase | State |
|-------|-------|
| t2a step A — ports | Done: ports bound once, swapped as snapshots; TSan 1,347 → 24 in the call suites |
| t1 — primitive | Done: owner threads in `AppRuntime` (Dedicated / Manual), affinity assert, naming hook, gate fix, THREADING.md rules. Nothing runs on them yet |

## Next

**t2a step B** — off-UI call entry points onto the media-sessions owner (inbound control, Accept / Leave / Decline, roster results, hop-migrate control tasks). Then **t2b**: lifecycle / bridge / seat / topology completions onto the owner with UI snapshots (the remaining TSan reports are these cross-owner reads).

## Known (motivating)

- ~~TSan: `BindWorkflowHostPorts` rewrote workflow ports under running callers~~ (t2a step A). Remaining 24: workflow reads lifecycle state (UI-owned) from workers; `CallLifecycle::ClearBinding` runs from the mesh-restart worker; `CallStack` teardown vs a MeshControl task.
- `InboundAttachGate::mu` taken by hop migrate, not by the topology writing the same fields.
- Sanitizers flag `AppRuntimeWorkerTest.ShutdownBudgetReturnsWhileWorkerBlocked` by design (it leaves a blocked worker detached past process exit); unchanged by t1.
