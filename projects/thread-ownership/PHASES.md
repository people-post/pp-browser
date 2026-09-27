# Thread ownership — phases

## t1 — Primitive + rules

- [x] `OwnerThread` (Dedicated / Manual) + `AppRuntime::PostTo` / `CurrentlyOn` / `ScheduleOn` / `RunOwnerTasks` / `RunAllOwnerTasks`; gate-wrapped; `DrainWorkersThenUI` sits behind owner queues, `QuiesceForTeardown` pumps Manual owners
- [x] `PBR_ASSERT_ON_OWNER(id)` debug affinity check
- [x] Thread naming via `AppRuntimeConfig::name_thread` (platform `os::SetCurrentThreadName`); `Application` + pp-node pass it
- [x] Teardown gate settles posts a mailbox drops unrun (a later quiesce used to wait out its budget)
- [x] Runtime tests `OwnerThreadTest` (both modes, cross-owner chains, timers, quiesce, dropped posts, affinity death test); TSan / ASan clean
- [x] THREADING.md: roles 7–8, § Owner threads, scheduling API, gate notes

## t2 — Media sessions owner

t2a (step A, done): ports bound once, swapped as snapshots.

- [x] `SharedPorts<T>` (mutex-guarded `shared_ptr<const T>`): session manager direct-media / lifecycle / seat ports, lifecycle signaling ports, topology + hop-migrate arming / seat ports. Each use takes one snapshot; setters no longer rebuild the workflow host ports (bound once in the ctor)
- [x] `CallStack::Lifecycle()` is an accessor and the ring callback no longer rebinds; binding happens at `BuildSessions` / `BindMediaProducts` (after mesh start) / cleared at mesh stop and `ResetSessions`
- [x] `PortSwapsDuringUseNeverCallAnEmptyPort` (old code segfaults); TSan reports in the call compose / backend suites 1,347 → 24 — the rest are cross-owner reads (workflow reading lifecycle state on UI; lifecycle `ClearBinding` from the mesh-restart worker) that only a shared owner fixes

t2a (step B, done): off-UI call entry points on the media-sessions owner ([T003](DECISIONS.md)).

- [x] Accept: `AcceptInviteAsync` — checks → async circuit park (`EnsureBootstrapSeedParkedAsync`, completion posted to the owner) → CallAccept + Joined + media arm; blocking `AwaitCircuitReady` removed
- [x] Lifecycle Accept / Decline / Leave, inbound call control, roster fan-out and hop-migrate flow steps post onto the owner (`PostToOwnerOrRun`)
- [x] Call test fixtures in Manual mode; `AcceptAwaitsCircuitParkWithoutBlockingTheOwner`
- [x] pp-call-probe's `send_user_message` is async like the product's (its blocking send parked the owner on a gone peer's ack → teardown UAF in hard-w5 STACK)
- [x] TSan 24 → 10 in the call suites (left: `CallStack` teardown vs the MeshControl peer-reach prefetch — t3); hard-w5 `all` green


- [ ] Call entry points onto the owner: inbound control, UI intents (`CallUiBackend` → post), lifecycle, worker results posted back; Accept / Leave / Decline split into owner steps + worker I/O
- [ ] Ports bound once on the owner; `CallStack::Lifecycle()` / ring-changed stop rebinding; drop the rebind race (TSan test: inbound accept vs mesh stop)
- [ ] Topology / bridge / hop migrate `PostUI` hops → owner; MeshControl hop-migrate posts → owner + worker
- [ ] Broadcast hub on the owner
- [ ] Affinity asserts on the moved classes

## t3 — Connectivity owner

- [ ] `MeshMediaPlane`, `PeerReachCoordinator`, `ReachabilityEngine` on the owner; candidate policy computed there (not on IO)
- [ ] Sync waits (`AwaitCircuitReady`, sync `TryEnsure*`) → completions; MeshControl retired

## t4 — UI snapshots

- [ ] Published call / broadcast snapshots; `CallUiBackend` queries read them
