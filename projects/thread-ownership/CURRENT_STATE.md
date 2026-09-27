# Thread ownership — current state

**Last updated:** 2026-09-27
**Branch:** `refactor/peer-reach-coordinator` (after media-client-layers l8)

## Landed

| Phase | State |
|-------|-------|
| t3-1 — MeshControl retired | Mesh waits are completions (dial-back walk, inbound call-media key, CAS tip fetch); L4 inbound work on workers; mesh stop joins MeshPump before freeing L4. TSan clean in call / broadcast / chat / mesh suites; hard-w5 green |
| t2b — media sessions on the owner | t2b-1 `CallsThread`; t2b-2 GUI boundary (intents post, `CallUiState` snapshot); t2b-3 calls flipped to the media-sessions owner, hub lifecycle edges via `RunAndWait`; t2b-4 broadcast on the same owner (`BroadcastUiState`, async announce). TSan 10 → 3 in the call suites, 0 in broadcast; hard-w5 green |
| t2a step B — entry points | Done: accept parks asynchronously; Accept / Decline / Leave, inbound control, roster fan-out, hop-migrate steps on the media-sessions owner; call tests in Manual mode; hard-w5 green |
| t2a step A — ports | Done: ports bound once, swapped as snapshots; TSan 1,347 → 24 → 10 in the call suites |
| t1 — primitive | Done: owner threads in `AppRuntime` (Dedicated / Manual), affinity assert, naming hook, gate fix, THREADING.md rules. Nothing runs on them yet |

## Next

**t3-2** — connectivity owner: `MeshMediaPlane`, `PeerReachCoordinator` (off the Coordinator strand), `ReachabilityEngine` onto it; the calls owner's `CallStackDeps` providers read a connectivity snapshot instead of the hub's `mesh()`.

## Known (motivating)

- ~~TSan: `BindWorkflowHostPorts` rewrote workflow ports under running callers~~ (t2a step A). ~~Remaining 3: `CallStack` teardown vs the MeshControl peer-reach prefetch~~ (t3-1: the prefetch posts the hub's port to UI).
- Call-side mesh media callbacks (`note_lan_mdns_peer_id` → hub set) still run on whichever thread the mesh media plane calls from — t3 with the plane.
- The calls owner reads hub state through `CallStackDeps` (`mesh()`, `config()`, directory / DHT snapshots) while the hub swaps `mesh_` on UI. Mesh stop clears the call ports first (`PrepareForMeshStop` waits on the owner), but provider lambdas (local peer id / caps / listen addrs) still reach `mesh()` — t3 gives them a connectivity snapshot.
- `InboundAttachGate::mu` taken by hop migrate, not by the topology writing the same fields.
- Sanitizers flag `AppRuntimeWorkerTest.ShutdownBudgetReturnsWhileWorkerBlocked` by design (it leaves a blocked worker detached past process exit); unchanged by t1.
