# Thread ownership — current state

**Last updated:** 2026-09-27
**Branch:** `refactor/peer-reach-coordinator` (after media-client-layers l8)

## Landed

| Phase | State |
|-------|-------|
| t2b — calls on the owner | t2b-1 `CallsThread`; t2b-2 GUI boundary (intents post, `CallUiState` snapshot); t2b-3 flipped to the media-sessions owner, hub lifecycle edges via `RunAndWait`. TSan 10 → 3 in the call suites; hard-w5 green |
| t2a step B — entry points | Done: accept parks asynchronously; Accept / Decline / Leave, inbound control, roster fan-out, hop-migrate steps on the media-sessions owner; call tests in Manual mode; hard-w5 green |
| t2a step A — ports | Done: ports bound once, swapped as snapshots; TSan 1,347 → 24 → 10 in the call suites |
| t1 — primitive | Done: owner threads in `AppRuntime` (Dedicated / Manual), affinity assert, naming hook, gate fix, THREADING.md rules. Nothing runs on them yet |

## Next

**t2b-4** — broadcast hub on the media-sessions owner (workflows' `post_ui` ports, facade intents, GUI snapshot). Then **t3** (connectivity owner; MeshControl peer-reach prefetch moves there).

## Known (motivating)

- ~~TSan: `BindWorkflowHostPorts` rewrote workflow ports under running callers~~ (t2a step A). Remaining 3 (call suites): `CallStack` teardown vs the MeshControl peer-reach prefetch (t3).
- Call-side mesh media callbacks (`note_lan_mdns_peer_id` → hub set) still run on whichever thread the mesh media plane calls from — t3 with the plane.
- The calls owner reads hub state through `CallStackDeps` (`mesh()`, `config()`, directory / DHT snapshots) while the hub swaps `mesh_` on UI. Mesh stop clears the call ports first (`PrepareForMeshStop` waits on the owner), but provider lambdas (local peer id / caps / listen addrs) still reach `mesh()` — t3 gives them a connectivity snapshot.
- `InboundAttachGate::mu` taken by hop migrate, not by the topology writing the same fields.
- Sanitizers flag `AppRuntimeWorkerTest.ShutdownBudgetReturnsWhileWorkerBlocked` by design (it leaves a blocked worker detached past process exit); unchanged by t1.
