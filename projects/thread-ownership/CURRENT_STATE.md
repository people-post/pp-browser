# Thread ownership — current state

**Last updated:** 2026-09-27
**Branch:** `refactor/peer-reach-coordinator` (after media-client-layers l8)

## Landed

| Phase | State |
|-------|-------|
| t2a step B — entry points | Done: accept parks asynchronously; Accept / Decline / Leave, inbound control, roster fan-out, hop-migrate steps on the media-sessions owner; call tests in Manual mode; hard-w5 green |
| t2a step A — ports | Done: ports bound once, swapped as snapshots; TSan 1,347 → 24 → 10 in the call suites |
| t1 — primitive | Done: owner threads in `AppRuntime` (Dedicated / Manual), affinity assert, naming hook, gate fix, THREADING.md rules. Nothing runs on them yet |

## Next

**t2b** — lifecycle / bridge / seat / topology completions onto the media-sessions owner, with UI snapshots for `CallUiBackend` queries; broadcast hub on the owner.

## Known (motivating)

- ~~TSan: `BindWorkflowHostPorts` rewrote workflow ports under running callers~~ (t2a step A). Remaining 10 (call suites): `CallStack` teardown vs the MeshControl peer-reach prefetch (t3). Known cross-owner reads (workflow ↔ lifecycle, UI-owned) remain until t2b even where TSan does not catch them in tests.
- `InboundAttachGate::mu` taken by hop migrate, not by the topology writing the same fields.
- Sanitizers flag `AppRuntimeWorkerTest.ShutdownBudgetReturnsWhileWorkerBlocked` by design (it leaves a blocked worker detached past process exit); unchanged by t1.
