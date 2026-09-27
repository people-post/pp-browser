# Thread ownership — decisions

## T001 — Four owners; calls and broadcast share one media-sessions thread

**Date:** 2026-09-27
**Status:** Accepted
**Decision:** Mesh I/O (MeshPump, as today: Amp drive + L4 protocol engines), Connectivity (new), Media sessions (new: calls **and** broadcast control), UI. Worker pool for blocking work; real-time media threads unchanged; coordinator shrinks to timers; MeshControl retires when sync waits are gone.
**Rationale:** Mesh I/O already is a protocol-engine thread and must stay one (channel sessions are IO-affine; lock order depends on it) — what leaks onto it is product policy, which moves to Connectivity. Calls and broadcast drive the same engine, device arbiter and relay client, so one media-sessions owner turns "call + broadcast at once" into scheduling instead of locking. Dedicated named threads (not strands on a pool): two new threads, readable TSan / stack traces, trivial affinity asserts.
**Consequence:** UI stops owning call state; `CallUiBackend` queries become snapshot reads (t4).

## T002 — Owner threads live in `AppRuntime`, behind the teardown gate, with a Manual mode for tests

**Date:** 2026-09-27
**Status:** Accepted
**Decision:** `AppRuntime::PostTo` / `CurrentlyOn` / `ScheduleOn` / `RunOwnerTasks` over `OwnerThread`s created in `Initialize`. Posts are gate-wrapped like every other runtime mailbox, so `QuiesceForTeardown` covers them. `AppRuntimeConfig::owner_threads` selects Dedicated (product) or Manual (tests: no OS thread, drained by the caller, `CurrentlyOn` true while draining). Thread naming is injected by the composition root.
**Rationale:** One place for every mailbox keeps teardown ordering and quiesce semantics uniform. Manual mode keeps owner-thread code as deterministic to test as UI code is today, without threading every harness.
