# Thread ownership — current state

**Last updated:** 2026-09-27
**Branch:** `refactor/peer-reach-coordinator` (after media-client-layers l8)

## Landed

| Phase | State |
|-------|-------|
| t4 — UI snapshots / config / asserts | Mesh config published as a snapshot for owners; UI reads only snapshots, intents and durable stores; LAN-peer note posts to UI; affinity asserts on owner-internal steps. TSan clean; hard-w5 green |
| t3-2c — reachability + local view on connectivity | `ReachabilityEngine` steps on the owner (UPnP on a worker); call providers read `MeshLocalView`; punch burst via the plane. TSan clean (whole mesh binary); ASan + hard-w5 green |
| t3-2b — candidate policy on connectivity | `MeshHopPolicy` snapshot evaluated on the owner (Wire / 5 s / `RefreshHopPolicy`); the IO side reads only the snapshot. TSan clean; hard-w5 green |
| t3-2a — connectivity owner: reach + mesh media plane | `PeerReachCoordinator` on it; `MeshMediaPlane` edges `RunAndWait`, listen book snapshot, relay-chosen / punch hooks hop owner to owner; T004 (waits downward only). TSan clean; hard-w5 green |
| t3-1 — MeshControl retired | Mesh waits are completions (dial-back walk, inbound call-media key, CAS tip fetch); L4 inbound work on workers; mesh stop joins MeshPump before freeing L4. TSan clean in call / broadcast / chat / mesh suites; hard-w5 green |
| t2b — media sessions on the owner | t2b-1 `CallsThread`; t2b-2 GUI boundary (intents post, `CallUiState` snapshot); t2b-3 calls flipped to the media-sessions owner, hub lifecycle edges via `RunAndWait`; t2b-4 broadcast on the same owner (`BroadcastUiState`, async announce). TSan 10 → 3 in the call suites, 0 in broadcast; hard-w5 green |
| t2a step B — entry points | Done: accept parks asynchronously; Accept / Decline / Leave, inbound control, roster fan-out, hop-migrate steps on the media-sessions owner; call tests in Manual mode; hard-w5 green |
| t2a step A — ports | Done: ports bound once, swapped as snapshots; TSan 1,347 → 24 → 10 in the call suites |
| t1 — primitive | Done: owner threads in `AppRuntime` (Dedicated / Manual), affinity assert, naming hook, gate fix, THREADING.md rules. Nothing runs on them yet |

## Next

All planned phases landed (t1–t4). Remaining: the items under Known; promote the settled rules (THREADING.md already carries them) and archive the project once the branch merges.

## Known (motivating)

- ~~TSan: `BindWorkflowHostPorts` rewrote workflow ports under running callers~~ (t2a step A). ~~Remaining 3: `CallStack` teardown vs the MeshControl peer-reach prefetch~~ (t3-1: the prefetch posts the hub's port to UI).
- ~~`note_lan_mdns_peer_id` → hub set from the plane's thread~~ (t4: posts to UI).
- ~~Owners read the hub's live `config()`~~ (t4: `MeshConfig` snapshot).
- ~~The calls owner's providers reached the hub's `mesh()` during call flows~~ (t3-2c: `MeshLocalView`). Bind-time reads of `mesh()` remain at the owner edges (the hub waits there).
- `InboundAttachGate::mu` taken by hop migrate, not by the topology writing the same fields — moot since t2b-3 (every user runs on the calls owner); the lock is now redundant and can go with an owner assert.
- Sanitizers flag `AppRuntimeWorkerTest.ShutdownBudgetReturnsWhileWorkerBlocked` by design (it leaves a blocked worker detached past process exit); unchanged by t1.
