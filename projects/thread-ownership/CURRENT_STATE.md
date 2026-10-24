# Thread ownership — current state

**Last updated:** 2026-09-27
**Branch:** `refactor/peer-reach-coordinator` (after media-client-layers l8)

## Landed

| Phase | State |
|-------|-------|
| t4 — UI snapshots / config / asserts (+ close-out: redundant attach-gate lock removed) | Mesh config published as a snapshot for owners; UI reads only snapshots, intents and durable stores; LAN-peer note posts to UI; affinity asserts on owner-internal steps. TSan clean; hard-w5 green |
| t3-2c — reachability + local view on connectivity | `ReachabilityEngine` steps on the owner (UPnP on a worker); call providers read `MeshLocalView`; punch burst via the plane. TSan clean (whole mesh binary); ASan + hard-w5 green |
| t3-2b — candidate policy on connectivity | `MeshHopPolicy` snapshot evaluated on the owner (Wire / 5 s / `RefreshHopPolicy`); the IO side reads only the snapshot. TSan clean; hard-w5 green |
| t3-2a — connectivity owner: reach + mesh media plane | `PeerReachCoordinator` on it; `MeshMediaPlane` edges `RunAndWait`, listen book snapshot, relay-chosen / punch hooks hop owner to owner; T004 (waits downward only). TSan clean; hard-w5 green |
| t3-1 — MeshControl retired | Mesh waits are completions (dial-back walk, inbound call-media key, CAS tip fetch); L4 inbound work on workers; mesh stop joins MeshPump before freeing L4. TSan clean in call / broadcast / chat / mesh suites; hard-w5 green |
| t2b — media sessions on the owner | t2b-1 `CallsThread`; t2b-2 GUI boundary (intents post, `CallUiState` snapshot); t2b-3 calls flipped to the media-sessions owner, hub lifecycle edges via `RunAndWait`; t2b-4 broadcast on the same owner (`BroadcastUiState`, async announce). TSan 10 → 3 in the call suites, 0 in broadcast; hard-w5 green |
| t2a step B — entry points | Done: accept parks asynchronously; Accept / Decline / Leave, inbound control, roster fan-out, hop-migrate steps on the media-sessions owner; call tests in Manual mode; hard-w5 green |
| t2a step A — ports | Done: ports bound once, swapped as snapshots; TSan 1,347 → 24 → 10 in the call suites |
| t1 — primitive | Done: owner threads in `AppRuntime` (Dedicated / Manual), affinity assert, naming hook, gate fix, THREADING.md rules. Nothing runs on them yet |

## Next

**Done.** All phases landed (t1–t4); rules promoted to [THREADING.md § Owner threads](../../docs/architecture/THREADING.md#owner-threads); decisions frozen (outcomes superseded by the stable doc); project archived in [projects/README.md](../README.md).

## Known (motivating)

- ~~TSan: `BindWorkflowHostPorts` rewrote workflow ports under running callers~~ (t2a step A). ~~Remaining 3: `CallStack` teardown vs the MeshControl peer-reach prefetch~~ (t3-1: the prefetch posts the hub's port to UI).
- ~~`note_lan_mdns_peer_id` → hub set from the plane's thread~~ (t4: posts to UI).
- ~~Owners read the hub's live `config()`~~ (t4: `MeshConfig` snapshot).
- ~~The calls owner's providers reached the hub's `mesh()` during call flows~~ (t3-2c: `MeshLocalView`). Bind-time reads of `mesh()` remain at the owner edges (the hub waits there).
- ~~`InboundAttachGate::mu` taken by hop migrate, not by the topology writing the same fields~~ — removed: every user runs on the calls owner (posted completions assert it).
- Sanitizers flag `AppRuntimeWorkerTest.ShutdownBudgetReturnsWhileWorkerBlocked` by design (it leaves a blocked worker detached past process exit); unchanged by t1.

## Changelog (from docs/architecture/THREADING.md)

| Date | Change |
|------|--------|
| 2026-10-19 | **One driver:** MeshPump claims the runtime (`ClaimDriver` / `ReleaseDriver` on its thread); pp-node's main loop no longer ticks beside it; the DHT self-publish is an Amp io tick and `ConfigureAmpDht` runs on the lane (it raced the tick) |
| 2026-10-19 | **Mesh control on the io lane:** `PeerReachCoordinator` (`PeerReachPorts` with its lane), `MeshConnectivity` / `MeshMediaRelay`, rendezvous parks and `ReachabilityEngine` run on `MeshHost::IoLane()` (pp-cpp-amp `IoExecutor`), timers on Amp's clock; a new lane fails reaches in flight and mesh stop answers pending parks at once (rule 4). `pp-connectivity` retired; `MeshHost::SetStopping` replaces the app's quit flag in the mesh; tests with fakes use `AppRuntime::MakeLaneForTesting` |
| 2026-10-19 | **Mesh executors:** each `MeshHost` takes `MeshExecutors` (`work` / `blocking`; pp-browser's `AppRuntimeMeshExecutors`); relay attach continues on `work`, the directory cache refreshes on `blocking` and no longer posts to UI |
| 2026-10-19 | **Mesh threads** decided ([§ Mesh threads](#mesh-threads-control-runs-on-the-io-lane)): control moves onto the I/O lane. First step: the app pushes the candidate policy (`MeshHopPolicyFeed`, on a worker); connectivity no longer calls the contacts store, directory or DHT |
| 2026-09-27 | **UI snapshots / config** (thread-ownership t4): owners read a published `MeshConfig`; affinity asserts on owner-internal steps |
| 2026-09-27 | **Connectivity owner, cont.** (thread-ownership t3-2b/c): `MeshHopPolicy` evaluated on connectivity; `ReachabilityEngine` steps there; call providers read `MeshLocalView` |
| 2026-09-27 | **Connectivity owner** (thread-ownership t3-2a): `PeerReachCoordinator` + `MeshMediaPlane` (since split into `MeshConnectivity` + `MeshMediaRelay`) on `pp-connectivity`; T004 downward-only waits |
| 2026-09-27 | **MeshControl retired** (thread-ownership t3-1): dial-back server walk, inbound call-media key wait and CAS tip fetch are completions; L4 inbound work on the worker pool; `MeshHost::StopAmp` joins MeshPump before freeing L4 |
| 2026-09-27 | **Broadcast on Media sessions** (thread-ownership t2b-4): hub intents post, `BroadcastUiState` snapshot, async announce onto the hub's thread; `AppRuntime::RunAndWait` |
| 2026-09-27 | **Calls on Media sessions** (thread-ownership t2b): `CallsThread` → `pp-media-sess`; GUI intents post, `CallUiState` snapshot; hub lifecycle edges via `CallsThread::RunAndWait` |
| 2026-10-21 | **One owner executor for runners**: `CallsThread` / `CallsExecutor` and broadcast's `MediaSessionsExecutor` → `RuntimeOwnerExecutor` (`foundation/runtime`), one per runner; broadcast's fallback without the owner is UI like calls' (was inline); after-task hooks are per executor (were process-wide) |
| 2026-09-27 | **Owner threads** (thread-ownership t1): `AppRuntime::PostTo` / `CurrentlyOn` / `ScheduleOn` / `RunOwnerTasks`, Dedicated / Manual modes, `PBR_ASSERT_ON_OWNER`, gate settles dropped posts. Mesh stop order is owned by the hub around `MeshMediaPlane` (now `MeshConnectivity` + `MeshMediaRelay`; media-client-layers L015), not `CallStack::StopMesh` |
| 2026-09-25 | **InboundReply:** worker-answered L4 requests (direct chat, history, blob, broadcast, announce, DHT, directory, dial-back) reply on the IO lane with the channel held open — acks were dropped under MeshPump |
| 2026-09-25 | **Idempotent Stop** for Amp L4 transports; `CallStack::StopMesh` single stop order; `MeshHost::AttachAmpStack(…, AttachDrive::MeshPump)` for wall-clock harnesses (pp-call-probe runs the product threading) |
| 2026-09-24 | **Teardown quiesce:** `AppRuntime` gate (Open → Draining → Closed, epoch reopen) over workers / coordinator / UI; quit and profile reset quiesce before freeing messaging; hub `shutdown_requested_` cleared on re-Initialize |
| 2026-09-23 | **Cancel / Abort contract:** arm ⇒ complete on cancel; Bridge `AbortConnectSequence` clears Connect waiter after canceling grace/retry timers |
| 2026-09-23 | **Exclusive Amp Drive:** nested Drive refused; `PostDeferred` / `PostAfter`; L4 `MakeL4IoPump` always empty; punch on `MeshRuntime&` via `BurstDial`; pin pp-cpp-amp `v2.1.8` |
| 2026-09-23 | DHT drop unused IoPump; directory sync via AmpParkUntil; `AmpScheduleUntilSettled` prefers PostAfter (`MeshIoContext.post_after`); never AmpParkUntil on Drive stack |
| 2026-09-23 | Punch ACP: mux handlers PostToIo only; async introducer (no AmpParkUntil under mux); burst on IO strand |
| 2026-09-21 | Amp link plane: LinkId + PeerPresence; WhenChannelOpen/BindChannel; completions via PostToIo; DialBook/LinkTable split; no product PeerLink* |
| 2026-09-09 | Shutdown latency phases 0–5: BeginShutdown+watchdog; budgeted coordinator/WorkerPool/ringtone/media joins; IsShuttingDown gates; dogfood matrix |
| 2026-09-09 | Shutdown latency: HideWindow on RequestExit; PrepareForTeardown(0); MeshControlPool join ≤500ms; shutdown timeline marks |
| 2026-09-09 | CallMediaBridge peer-reach Async; CallSessionManager SoftMigrate nudge uses SoftMigrateAsync (no Worker park) |
| 2026-09-09 | Circuit hop TryEnsure*Async + CallStack punch Async; SoftMigrate/Attach/Reattach await dialability Async |
| 2026-09-09 | SoftMigrate/AttachLocalToSfu/ReattachGuest Async — MeshControl no longer parks on media-relay quote/attach |
| 2026-09-09 | Media-relay RequestQuoteAsync/AcceptAndAttachAsync; CallStack uses MeshHost io_pump (no Tick-from-waiters) |
| 2026-09-09 | Reachability probe async (ProbeAsync + PostToIo); announce PublishAndPush*Async; MeshControl no longer parks on seed dial |
| 2026-09-09 | Amp MeshPump + MeshControlPool owned by MeshHost; coordinator no longer 5ms Amp drain; docs drop libp2p reactor assumptions |
| 2026-08-03 | Call Accept layer: `RemountCallChrome` (dedicated mounts); not always-mounted `data-if` + Dirty alone |
| 2026-08-03 | Relay poll owned by `ConversationsHub::StartCoordinatorTimers` (not ChatController WireMessagingBindings); immediate wake sync on arm; `SetOnCallWake` from Application |
| 2026-08-03 | **UI delivery:** `PostTask(UI)` → `RequestForceFrame`; idle = Poll+≤50ms Delay (no WaitEventTimeout); mid-idle abort on ForceFrame/wake; liveness contract in Cross-thread rules |
| 2026-08-03 | Call chrome + UI mailbox: hop ring refresh to UI; `RequestForceFrame` when UI tasks pending / SyncLayout; WakeEventLoop always pushes (no coalesce-drop); unanswered outbound TTL |
| 2026-08-03 | **Shipped:** coordinator + worker pool model live; `pp-browser-io` retired; project folder archived |
| 2026-08-03 | Phase t5: `BrowserThread::IO` → worker pool |
| 2026-08-03 | Retire `BrowserThread`; UI mailbox lives on `AppRuntime` |
| 2026-08-03 | Phase t4: `CoordinatorThread` + timer wheel |
| 2026-08-03 | Phase t3/t3.5: messaging hop-offs + `AppRuntime` |
| 2026-08-03 | Phase t2: libp2p integration hop-offs |
| 2026-08-03 | Phase t1: `WorkerPool` in `src/common/` |
