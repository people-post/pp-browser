# Thread ownership — design

> Historical: the starting picture and target model. Delivered — see [THREADING.md § Owner threads](../../docs/architecture/THREADING.md#owner-threads) for how the system is now (MeshControl below no longer exists; reach runs on Connectivity).

## Today (2026-09-27)

| Thread | Runs | Problem |
|--------|------|---------|
| Amp MeshPump (Mesh I/O) | `MeshRuntime::Drive`, links, mux, **all L4 protocol state machines** (circuit, media_relay, call_media leg, punch, dial-back, DHT, directory, chat / history / announce / broadcast / blob inbound) | Product policy leaks in: rendezvous parking asks the product for candidates (contacts store, directory cache, DHT snapshot) on IO |
| Coordinator | timers, relay poll, hub policy, **`PeerReachCoordinator`** | Reach policy on a dispatcher meant to be "fast policy only" |
| MeshControl pool | sync waits parked while MeshPump drives (probe, punch, chat waits, hop migrate) | Exists because product code blocks waiting for the mesh |
| Worker pool | HTTP, SQLite, Argon2, LLM — and call Accept / Leave / Decline, roster fan-out | Call state mutated from workers |
| Inbound control path | `ApplyInboundControl` → call workflow | Call state mutated from receive threads |
| UI | chrome, controllers — and bridge / topology ("UI-only" by `PostUI` hops), `CallStack::Lifecycle()` rebinding ports on every access | UI both owns and races call state |

Call state is touched from UI, worker Critical / Normal, inbound control, MeshControl and the coordinator; protection is per case (hops, a few mutexes — one used inconsistently — generation tokens for lifetime only).

## Target

| Owner (serialized, named) | Owns | Never |
|---|---|---|
| **Mesh I/O** (MeshPump) | Amp drive, links, mux, L4 protocol engines, inbound frame dispatch | product policy, product stores |
| **Connectivity** | `MeshMediaPlane` lifecycle, dial registry + listen book, circuit reach orchestration, `PunchIntroducerWalk`, `CircuitRendezvousCoordinator` decisions, `PeerReachCoordinator`, `ReachabilityEngine` | blocking; Amp mutations go to Mesh I/O by post |
| **Media sessions** | calls (session manager, workflow, topology, hop migrate, lifecycle, bridge policy, seat) and broadcast (hub, viewer, broadcaster); engine / arbiter control calls | blocking I/O — SQLite, relay HTTP go to workers |
| **UI** | presentation; reads published snapshots, posts intents | owning call / connectivity state |

Unchanged: **worker pool** (all blocking work), **real-time media threads** (capture / playout / video / device), **coordinator** (shrinks to timers + wakeups posting onto owners). **MeshControl** retires once sync waits become completions.

### Rules

1. **One owner per object.** Public methods assert they run on the owner (debug builds abort with the call site) or post to it.
2. **Messages between owners.** Posted closures carry values; shared views are immutable snapshots (`shared_ptr<const T>`). A port is "post to that owner" — bound once, on the owner, invoked there. The rebind race disappears as a class.
3. **Owners never block.** Blocking work goes to workers; the result is posted back.
4. **Data plane bypasses owners.** Capture → Amp send and Amp receive → playout stay direct (IO lock), as today. Only control goes through owners.
5. **Destroy on the owner.** `DeferredSelf` stays for queued callbacks.
6. **Lock order unchanged:** Amp strand → L4 owner mutex. Owners hold no lock while posting.

### Primitive (t1)

`AppRuntime` hosts the owner threads next to the UI mailbox, behind the same teardown gate:

- `AppRuntime::PostTo(OwnerThreadId, task)`, `CurrentlyOn(id)`, `ScheduleOn(id, delay, task)` (coordinator timer → post), `RunOwnerTasks(id)`.
- `OwnerThread` — a named thread + FIFO. **Dedicated** mode (product): its own OS thread. **Manual** mode (tests, harnesses): no thread; tasks run when the driving thread calls `RunOwnerTasks` (like `RunUITasks`), and `CurrentlyOn` is true while they do. `DrainWorkersThenUI` / `QuiesceForTeardown` pump manual owners.
- `PBR_ASSERT_ON_OWNER(id)` — debug-build affinity check.
- Thread names come from the composition root (`AppRuntimeConfig::name_thread`, platform `os::SetCurrentThreadName`) — `foundation/runtime` has no OS code.

### Tests

Owner-thread code is tested in Manual mode by default: deterministic, drained by the test (`RunOwnerTasks` / `DrainUntil` helpers that pump every manual owner). Dedicated mode is covered by the runtime's own tests, TSan runs and the hard lab.

### GUI boundary (t2b)

`CallUiBackend` is the only way the GUI touches calls: **intents** post to the calls owner (results on UI through `on_done`); **owner state** comes from `CallUiState`, which each `CallStack` publishes after every owner task (`CallsThread` after-task hooks) — a reader sees state as of the owner's last completed step, never mid-step; **durable state** (invites, sessions, participants) reads the stores; `Media()` is the engine, which synchronizes itself. GUI callbacks (chrome refresh, ring changed) always run on UI.

## Open questions

- Messaging (delivery, sync) is out of scope; it may get its own owner later.
