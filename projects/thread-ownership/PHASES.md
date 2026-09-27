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

t2b — the rest of the call stack onto the owner, one commit per step so every step builds and passes:

- [x] t2b-1: `CallsThread` — the one place that says which thread owns call state. Every internal "continue on the owner" hop (bridge, connect coordinator, seat, topology, hop migrate, lifecycle steps, and the t2a entry points) goes through it; GUI / hub notifications post to UI explicitly (`NotifyChrome`, the GUI ring callback wrapped by `CallUiBackend`, mobile listen sync). Still UI-backed: behaviour-neutral, and until the flip all call state is on one thread
- [x] t2b-2: `CallUiBackend` — intents post to the calls owner (results with `on_done` on UI: StartCall, InviteParticipant, mute, camera); owner state is read from `CallUiState`, published by each call stack after every owner task (`CallsThread` after-task hooks) and at bind points; durable state reads the stores (`PeekTopPendingInvite` filters expired rows instead of sweeping); the camera's display rotation is read on UI and passed along (L012); the bridge's attempted-calls set is guarded. `CallController` adapted (weak lifetime token for late results)
- [x] t2b-3: `CallsThread` flipped to the media-sessions owner (UI only while the runtime has no owner). The hub's lifecycle edges (`InitializeStores`, `BuildSessions`, mesh start / stop, `Detach` / `RebindMeshMedia`, `ResetSessions`, `AbortCallMediaForShutdown`, `Shutdown`, and hub / probe wiring through `CallStack::RunOnOwner`) run there via `CallsThread::RunAndWait` — the caller waits; inline when already on the owner, without an owner, or when the teardown gate drops the post (the caller then stands in for the owner). `Shutdown` drains workers from the caller between its two owner steps. Hub reads (`WantEphemeralListen`, `HasActiveLocalCall`, `IsConnectWorkerInflight`, `CallUiBackend::Available` / `SessionsIdentity`) come from `CallUiState`; the listen desire publishes before the hub's N025 sync is posted to UI. `CallLifecycle` is created with the stack (no lazy create from UI). The two SoftMigrate / publisher re-fan-out timers hop to the owner. Found on the way: the engine outlived the bridge with its send / state callbacks still pointing into it — `CallStack` stops the engine before clearing the plane, the bridge clears its state callback. TSan 10 → 3 in the call suites (left: the MeshControl peer-reach prefetch — t3); hard-w5 green
- [ ] t2b-4: broadcast hub on the owner — viewer / broadcaster workflows' `post_ui` ports retarget, facade intents post, the GUI reads a snapshot


- [x] Call entry points onto the owner: inbound control, UI intents (`CallUiBackend` → post), lifecycle, worker results posted back; Accept / Leave / Decline split into owner steps + worker I/O (t2a, t2b-2)
- [x] Ports bound once on the owner; `CallStack::Lifecycle()` / ring-changed stop rebinding; drop the rebind race (t2a step A, t2b-3)
- [x] Topology / bridge / hop migrate `PostUI` hops → owner (t2b-1 / t2b-3)
- [ ] Broadcast hub on the owner (t2b-4)
- [ ] Affinity asserts on the moved classes

## t3 — Connectivity owner

- [ ] `MeshMediaPlane`, `PeerReachCoordinator`, `ReachabilityEngine` on the owner; candidate policy computed there (not on IO)
- [ ] Sync waits (`AwaitCircuitReady`, sync `TryEnsure*`) → completions; MeshControl retired

## t4 — UI snapshots

- [ ] Published call / broadcast snapshots; `CallUiBackend` queries read them
