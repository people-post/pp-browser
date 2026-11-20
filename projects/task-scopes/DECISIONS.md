# Task scopes: decisions

## TS001 — Every posted task belongs to a task scope

**Date:** 2026-11-20
**Status:** Proposed (2026-11-20), for Hongwei's review; after release 1. When it ships (T5), it amends
[THREADING § Teardown quiesce](../../docs/architecture/THREADING.md#teardown-quiesce) (the line "do not add
per-owner gates for teardown"), [OWNERSHIP § Lifetime](../../docs/architecture/OWNERSHIP.md#lifetime-ticket--invalidate),
and [thread-ownership T002](../thread-ownership/DECISIONS.md) (owner threads "behind the teardown gate").

**Decision:**

- Every owner that posts holds a `TaskScope` ([DESIGN § 2](DESIGN.md#2-target-model)). The scope holds the
  owner's home sequence (UI, an owner thread, or a pooled sequence on the shared `WorkerPool`), a weak handle and
  a pending count. Posts, timers and blocking jobs go through it.
- The owner's state is read and written only on its home. Other owners reach it with messages through weak
  handles (`ScopedRef`).
- Blocking work runs as jobs whose work captures values and services, never the owner. The reply comes back on
  home through the scope.
- Closing the scope, on home, makes queued tasks, timers and replies no-ops. Running jobs finish, with the cancel
  token set. The owner is freed only once its scope is Closed (two-step close when jobs are in flight).
- Teardown, profile reset and mid-life restarts close owners one by one. The global `QuiesceForTeardown` remains
  only as a process-exit backstop.
- A CI check rejects new raw-`this` posts outside a scope. Today's sites are allowlisted, and their counts only go
  down.

**Rationale:**

- **Safety by structure, not by order.** An owner freed without a global quiesce is still safe (PR #676's
  use-after-free cannot happen). A stale task cannot reach another owner's memory, because it is dropped with its
  own scope.
- **Narrow failures.** A stuck job blocks only its owner's close. Quit leaks that owner, not all of messaging.
  Profile reset fails only if a job of the profile's subtree cannot be cancelled.
- **Mid-life restart.** Per-owner cancellation makes destroying and recreating a subsystem safe: a relay client,
  a per-account hub, the mesh on a rewire.
- **One mechanism** instead of three (global gate, `pp::Lifetime`, `OwnerTasks`) chosen by convention. It
  generalises `OwnerTasks`, which already works this way on the media-sessions thread.
- **Fewer locks.** State confined to one sequence needs no locks or atomics. The hub has 18 mutexes and 7
  atomics today.

**Alternatives rejected** (details in [DESIGN § 7](DESIGN.md#7-alternatives-considered)):

- Status quo plus a per-suite test helper.
- One `pp::Lifetime` per owner: it runs on the caller's thread, so owner state stays multi-threaded, and it waits
  in destructors.
- `shared_ptr` / `enable_shared_from_this` on owners: destruction runs on any thread, teardown order becomes
  non-deterministic, and it conflicts with parent-only destroy.
- A full actor framework or `std::execution`: not in our toolchains, and a new model across the tree. Its ideas
  are kept.

### Open questions for Hongwei

Each question has a recommendation. Until you decide, the recommendation is what the design assumes.

| # | Question | Recommendation |
|---|----------|----------------|
| Q1 | Where does `TaskScope` live: pp-cpp-common `common/runtime/` (next to `OwnerTasks`, used by pp-ledger too), or pp-browser's base? | **pp-cpp-common**, as a minor version. The sequences have to live where `AppRuntime` and `WorkerPool` live. |
| Q2 | Should `OwnerTasks` be folded into `TaskScope` (one mechanism), or kept beside it? | **Fold it.** `OwnerTasks` becomes a `TaskScope` on an owner sequence (an alias in T0, renamed in T3). `CallsLoop` and `BroadcastHub` keep their shape. |
| Q3 | Which home does `ConversationsHub` get: UI (today's "hub's thread"), or a pooled "messaging" sequence of its own? | **UI first.** T2 then changes no UI-facing timing. Revisit after T2b, when its blocking parts are jobs; move it to a pooled sequence if UI stalls show up in traces. |
| Q4 | May a job's work touch the owner's members, with two-step close waiting for it, or must it capture only values and shared services? | **Values and services only, as the rule.** Two-step close with counted jobs is for the cases a PR names (the job owns a file the reset deletes). The check flags `this` in job work. |
| Q5 | When an owner is not Closed by quit's 2 s budget, do we leak that owner and its ancestors, or wait? | **Leak them** after `FlushForExit`, logging the scope names. That matches today's leak, narrowed to one owner. The 3 s watchdog stays the last resort. |
| Q6 | May a pooled home do short blocking work (a SQLite write), or must every blocking call be a job? | **Allow short disk work** (a few ms, documented per owner). Network and crypto are always jobs. |
| Q7 | Keep the coordinator thread? | **Yes, for now.** It stays the process timer wheel that fires scope timers, and the home of the background-sync policy. Folding it into pooled sequences is a later decision. |
| Q8 | How strict should enforcement be? | **From T1, no new raw-`this` posts** (the allowlist counts down). **From T5, no direct `AppRuntime::Post*` in product code** outside the composition root and the runtime adapters. |
| Q9 | Should anything land before release 1? | **T0 and T1 may land before it**: a library addition with no product user, and a lint that freezes today's counts. Neither changes runtime behaviour. **T2 onwards waits for release 1.** Separately, merge PR #676 now as the test fix it is. |
| Q10 | Should profile reset keep the global quiesce during migration? | **Yes, until T4 exits:** close the scoped owners first, then quiesce globally as the backstop for allowlisted sites. Drop `ReopenAfterTeardown` from reset once the allowlist is empty. |
