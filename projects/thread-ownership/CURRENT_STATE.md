# Thread ownership — current state

**Last updated:** 2026-09-27
**Branch:** `refactor/peer-reach-coordinator` (after media-client-layers l8)

## Landed

| Phase | State |
|-------|-------|
| t1 — primitive | Done: owner threads in `AppRuntime` (Dedicated / Manual), affinity assert, naming hook, gate fix, THREADING.md rules. Nothing runs on them yet |

## Next

**t2** — media-sessions owner: call entry points (inbound control, UI intents, lifecycle, worker results) onto the owner; ports bound once there; hop-migrate / topology / bridge hops retargeted; broadcast hub on the owner; tests in Manual mode.

## Known (motivating)

- TSan: `CallSessionManager::BindWorkflowHostPorts` rewrites workflow ports while call workers invoke them (~1.3k reports in `CallSessionInboundComposeTest` / `CallUiBackendStackTest`); `notify_ring_changed` → `EnsureCallLifecycleBound` replaces the `std::function` that is running.
- `InboundAttachGate::mu` taken by hop migrate, not by the topology writing the same fields.
- Sanitizers flag `AppRuntimeWorkerTest.ShutdownBudgetReturnsWhileWorkerBlocked` by design (it leaves a blocked worker detached past process exit); unchanged by t1.
