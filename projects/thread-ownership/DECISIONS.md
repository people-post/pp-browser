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

## T003 — Calls entry points on the media-sessions owner; sends prepare + enqueue; accept parks asynchronously

**Date:** 2026-09-27
**Status:** Accepted (t2a)
**Decision:**
- **Sends never block the sender.** A call-control send prepares on the sending owner (encrypt, store append) and enqueues: Amp on Mesh I/O with a completion callback, the relay (HTTP) fallback on a worker; the delivery outcome is recorded later. `MeshDeliveryOrchestrator::SendUserMessage` already works this way; every `send_user_message` implementation must (pp-call-probe's blocking send parked the owner on a gone peer's ack — hard-w5).
- **Accept parks asynchronously.** The circuit park before `CallAccept` is `EnsureBootstrapSeedParkedAsync`, continuing on the owner; the blocking `AwaitCircuitReady` is gone.
- **Entry points post onto the owner:** inbound call control (fire-and-forget with a copy — `ApplyInboundControl` only reads the message; failures are logged there, no longer reported as receive failures), lifecycle Accept / Decline / Leave (reply to UI), roster fan-out, hop-migrate flow steps (were MeshControl).
- **Tests** run owners in Manual mode (`ManualOwnerRuntimeConfig`, `RunUIAndOwnerTasks`).
**Rationale:** The owner may only do non-blocking work, and the only real blocking in the call flows was the park wait; the send path was already asynchronous.
**Consequence:** UI-owned call pieces (lifecycle, bridge, seat, topology completions) still share state with the owner until t2b.


## T004 — Waits between owners go downward only; mesh waits are completions

**Date:** 2026-09-27
**Status:** Accepted (t2b-3, t3-1, t3-2)
**Decision:**
- **Hierarchy:** UI → Media sessions → Connectivity → Mesh I/O. A component's lifecycle edge (build, rewire, reset, teardown) may `AppRuntime::RunAndWait` on an owner **below** its caller: the hub (UI) on the calls owner (`CallsThread::RunAndWait`) or on connectivity (`MeshMediaPlane` edges); the calls owner on connectivity (plane hooks). Upward is always a post (results, notices, hub callbacks). Nothing below waits on anything above, so a wait cannot close a cycle.
- **Steady-state traffic never waits:** intents, notices and completions are posts; `RunAndWait` is for the edges where the caller must know the owner's objects are rebound or gone before it continues (L015 sequencing, destruction).
- **No thread waits on the mesh.** Dials, associations, probes, key arrivals and blob fetches are completions (`EnsureAssociation` callbacks, `PostAfter` deadlines, parked continuations on the owner). MeshControl is retired (t3-1); `AmpParkUntil` remains only where the waiter is the sole Amp driver (manual-drive test harnesses).
- **Dropped posts:** when the teardown gate drops a `RunAndWait` post, the caller runs the step itself and counts as the owner (`CurrentlyOn`) for its duration.
**Rationale:** Owners stay responsive (no blocking) while the few ordering-critical edges keep synchronous semantics; a fixed direction makes deadlock structurally impossible rather than something each call site must argue.
