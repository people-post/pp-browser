# Task scopes: design

**Status:** proposed ([TS001](DECISIONS.md#ts001--every-posted-task-belongs-to-a-task-scope)), nothing built.
**Changes:** threading and teardown only. No wire, disk or HTTP format changes, so the release-1 compatibility
freeze ([release-compatibility](../release-compatibility/)) does not apply. The project is planned for after
release 1 because it touches core threading.

Code is cited as `path:line` at pp-browser `origin/main` 197eb3a9d (2026-11-20). The runtime is in pp-cpp-common
at the pinned tag **v1.4.0** (`cmake/PpCppCommon.cmake:19`) and the mesh in pp-cpp-mesh **v9.0.0**
(`cmake/PpCppMesh.cmake:20`). Paths in those repos are written `common:` and `mesh:`.

---

## 1. Today

### 1.1 The model

- **Mailboxes are process-wide.** `AppRuntime` is a static facade over one UI queue (`SequencedTaskRunner`, drained
  each frame: `common:include/common/SequencedTaskRunner.h:145`), one 2–4-thread `WorkerPool` with three lanes
  (`common:include/common/WorkerPool.h:123`), one coordinator (mailbox and timer wheel) and the owner threads
  (`common:include/common/runtime/AppRuntime.h:101-191`). Anyone may post anything to any of them.
- **Owners post closures that capture raw `this`.** On main there are 246 call sites of the form
  `AppRuntime::Post…/Schedule…([this|&…` in 33 non-test files, out of 400 `AppRuntime` post or schedule calls in 61
  files. This is a first-line grep, so lambdas whose capture list starts on the next line are missed; T1 measures
  the sites exactly. The largest files are `MeshDeliveryOrchestrator.cpp` (48), `ConversationsHub.cpp` (45),
  `ChatController.cpp` (32), `Application.cpp` (13), `ShellHost.cpp` (11), `SettingsSecurityActions.cpp` (11) and
  `CallController.cpp` (10).
- **Memory safety rests on one global ordering invariant.** One gate covers every mailbox (`GateTask`,
  `common:src/runtime/AppRuntime.cpp:253`; rationale at `:148-151`). Teardown calls
  `AppRuntime::QuiesceForTeardown(budget)` (`:443-489`). It moves the gate to Draining (queued tasks still run,
  posts from outside are dropped), waits until nothing gated is pending or running, and then moves it to Closed:
  everything no-ops. Only after that may owners be freed ([THREADING § Teardown
  quiesce](../../docs/architecture/THREADING.md#teardown-quiesce)). The same section says "do not add per-owner
  gates for teardown", and [OWNERSHIP § Lifetime](../../docs/architecture/OWNERSHIP.md#lifetime-ticket--invalidate)
  repeats it.
- **A second mechanism covers owners that stop mid-life.** That is `pp::Lifetime`, a generation ticket
  (`common:include/common/Lifetime.h:27`). `Bind` wraps a callable that no-ops after `Invalidate` (`:52`, `:65`),
  and `InvalidateAndWait` waits for bound calls running on other threads (`:55`). Its use is limited to a whitelist
  (OWNERSHIP.md:95-106). Which mechanism an owner uses is a matter of convention.
- **Owner runners already work the target way, on one thread.** `OwnerTasks` (`common:include/common/runtime/
  OwnerTasks.h:268`) is one object's posts and timers on an owner thread, all dropped with the object. A shared
  `Handle` outlives it and no-ops once it is closed (`common:src/runtime/OwnerTasks.cpp:16-47`). `CallsLoop`
  (`capabilities/calls/src/feature/calls/CallsLoop.h:57`) and `BroadcastHub`
  (`capabilities/broadcast/src/feature/broadcast/BroadcastHub.h:120`) are built on it, and
  `scripts/check/check_owner_runners.sh:27-46` keeps posting inside the runners. That model covers the
  media-sessions thread only, and it lacks two things: a pool sequence, and a count of work in flight off the
  owner.

### 1.2 A representative owner: `ConversationsHub`

All paths below are in `capabilities/messaging/src/feature/conversations/`.

- **It posts raw `this` 45 times.** The command
  `grep -nE "AppRuntime::(PostUI|PostWorker[A-Za-z]*|Schedule[A-Za-z]*)\(\s*\[this" ConversationsHub.cpp`
  finds them. Some nest: a UI task posts a worker task (`ConversationsHub.cpp:393-401`), and a coordinator
  one-shot posts a worker task (`:2320-2321`, `:2663-2664`). Four repeating coordinator timers capture `this`
  (`:2021-2037`). The process singleton `BackgroundSyncScheduler` holds a `[this]` handler (`:2040`, cleared at
  `:2073`).
- **7 posts or callbacks are bound to a `pp::Lifetime`:** `:2150`, `:3417`, `:3421`, `:3426`, `:3436`, `:3441` and
  `:3452`. They use three Lifetimes (`ConversationsHub.h:901`, `:996`, `:1014`), each invalidated with
  `InvalidateAndWait` in `Shutdown` (`ConversationsHub.cpp:4151`, `:4158`, `:4159`).
- **Its code runs on several threads.** "The hub's thread" is UI (`:437`). Worker tasks call hub members
  (`:401`, `:426-433`), and the coordinator runs `TickMesh` (`:2021`). The header marks many entry points "Any
  thread" (`ConversationsHub.h:177`, `:273`, `:276`, `:295`, …). The header therefore holds 18 mutexes and 7
  atomics, among them `shutdown_requested_` (`:1027`). That flag is checked at the top of almost every posted body
  (e.g. `ConversationsHub.cpp:394`, `:427`, `:438`).
- **Tests run without a runtime**, so 12 sites use "post if the runtime is running, else run inline"
  (`AppRuntime::IsRunning()`, e.g. `:138`, `:3143`, `:3168`).
- **The shutdown path:**
  1. `RequestShutdown` sets the flag and aborts the participants and the mesh's circuit waits (`:1063`).
     `CancelShutdownRequest` undoes it (`:1073`).
  2. `Application::Shutdown` runs the global quiesce (`src/app/Application.cpp:3174-3177`, 2 s budget
     `kQuitQuiesceBudget`, `:205`). Only then does `ShutdownMessaging` call `ConversationsHub::Shutdown`
     (`Application.cpp:503-514`, `ConversationsHub.cpp:4140`).
  3. When the quiesce misses its budget, `FlushForExit` (`ConversationsHub.cpp:4083`) persists, and the whole
     product graph is leaked: `(void)product_.release()` (`Application.cpp:3184-3185`).
  4. Profile reset (`StopMessagingForReset`, `Application.cpp:673`) runs the same quiesce with a 10 s budget
     (`:207`). When it misses, it reopens the gate, cancels the shutdown request and returns "Background work is
     still running — try again" (`:685-691`). On success it re-opens at `:711`.
- **The destructor** shuts the transfer lane down, then calls `Shutdown()` (`ConversationsHub.cpp:249-252`).

### 1.3 `CallStack`

`CallStack` (`capabilities/calls/src/feature/calls/CallStack.cpp`) is already a runner on the media-sessions owner.
Its lifecycle edges are `RunAndWait` (`:44-57`), and the loop closes last, on the owner (`:31-41`). Two parts still
use global mechanisms:

- `Shutdown` waits on the process-wide barrier `AppRuntime::DrainWorkersThenUI(2 s)` so that "LeaveCall /
  DeclineInvite workers" finish while the sessions live (`:785-799`). Those workers are delivery's
  (`capabilities/delivery/src/domain/delivery/ControlLane.cpp:242`, which is bound to its own `Lifetime`, and
  `DeliveryOutbox.cpp:138-140`, `:254-256`). The barrier waits for every owner's work, not only the calls'.
- Its chrome refresh posts to UI through a separate `pp::Lifetime chrome_self_` (`CallStack.h:316`,
  `CallStack.cpp:752`).

### 1.4 Limits

| # | Limit | Evidence |
|---|-------|----------|
| L1 | **Safety comes from time and order, not structure.** Freeing an owner is safe only after the global quiesce. Nothing stops code from freeing one without it. | PR #676 (open): `HubParticipantTest` built hubs on the stack without draining. A raw-`this` UI post from `RecheckRelayIdentitiesSoon` (`ConversationsHub.cpp:437`) ran in a later test against a dead frame (ASan stack-use-after-return; 16 of 30 runs crashed). The fix is a test-only `TestHub` that drains in its destructor. |
| L2 | **One owner's stale task runs against another owner's memory.** The mailboxes are shared, and a task does not know whose it is. | Same PR: "the stale task wrote into the new test's hub, which sat in the same stack slot". |
| L3 | **Teardown memory safety depends on budgets.** | Quit leaks all of messaging when 2 s are not enough (`Application.cpp:3181-3185`). Profile reset can fail with "try again" (`:685-691`). One slow job anywhere blocks every owner's teardown. |
| L4 | **No owner can be cancelled on its own, so no subsystem can be destroyed and recreated mid-life.** A per-account hub, or restarting a relay client, would need a global quiesce of everything. | The quiesce is process-wide, and `ReopenAfterTeardown` bumps one global epoch (`common:src/runtime/AppRuntime.cpp:491-498`). |
| L5 | **There are two mechanisms, and convention picks between them.** | The global gate, `pp::Lifetime` (whitelist OWNERSHIP.md:95-106), `OwnerTasks` (runners), and the flag `shutdown_requested_`. The hub uses all of them except `OwnerTasks`. |
| L6 | **Owner code runs on several threads, so owner state needs locks and atomics.** | The hub has 18 mutexes and 7 atomics (§ 1.2). THREADING.md's lock order rules (`Amp strand → L4 owner mutex`) exist because of this. |
| L7 | **Some work sits outside the gate.** | `AiTurnPool` is not behind it, and `AgentSessions` joins its turns itself (THREADING.md:65). Profile reset must stop the turns first (`Application.cpp:675`). |

---

## 2. Target model

### 2.1 Vocabulary

- **Sequence:** an executor that runs its tasks one at a time, in post order, never two at once. Four kinds, all
  implementing the existing `pp::SerialExecutor` (`common:include/common/SerialExecutor.h:198`):
  - the **UI sequence** (the UI mailbox, as today);
  - an **owner sequence** (an owner thread, as today);
  - a **pooled sequence** (new: a queue that borrows one `WorkerPool` thread at a time; no new thread);
  - a **manual sequence** (tests: drained by the test, as `MakeLaneForTesting` is today, `AppRuntime.h:213`).

  The mesh's io lane (`mesh:include/mesh/host/MeshHost.h:106`, `IoLane()`) is already a `SerialExecutor`.
- **Home:** the sequence an owner's state lives on. All of an owner's state is read and written there only, the
  way an actor works. This removes the locks of L6.
- **TaskScope:** a member of the owner. It holds the home sequence, a weak handle others can hold, a pending count
  (posted and not yet run or dropped, plus jobs running off the home sequence) and a cancel flag.
- **Job:** blocking work (network, disk, crypto) that the scope runs on a worker lane. Its reply is posted back to
  the home sequence through the scope.

### 2.2 API sketch (pp-cpp-common `common/runtime/TaskScope.h`)

```cpp
namespace pp {

// Sequences. All are SerialExecutor (Post, PostAfter, CancelTimer, RunAndWait, RunsTasksOnCurrentThread).
std::shared_ptr<SerialExecutor> AppRuntime::UISequence();
std::shared_ptr<SerialExecutor> AppRuntime::OwnerSequence(OwnerThreadId owner);
/** A serial lane on the pool: one task at a time, on whichever pool thread is free. No new thread. */
std::shared_ptr<SerialExecutor> AppRuntime::NewPooledSequence(std::string name, WorkerLane lane);

/** Set when the scope closes. Long jobs poll it (curl progress callback, loop step); any thread. */
class CancelToken {
public:
  bool Cancelled() const;
};

class TaskScope {
public:
  TaskScope(std::shared_ptr<SerialExecutor> home, std::string name);
  /**
   * On home (a manual sequence: on the thread that drains it). Closes the scope if it is open. If jobs are still
   * in flight, debug builds abort, naming the scope; release builds wait up to kDtorBudget, then log and leak
   * the jobs' state. Declared as the owner's last member, so it goes first.
   */
  ~TaskScope();

  // --- own work, on home -------------------------------------------------------------------------
  void Post(std::function<void()> task);           // any thread; no-op once closing
  void PostFront(std::function<void()> task);      // for a documented ordering only
  TimerId PostAfter(std::chrono::milliseconds delay, std::function<void()> task);
  void Cancel(TimerId& id);
  /** A timer re-armed after each run; cancelled with the scope. */
  TimerId PostEvery(std::chrono::milliseconds interval, std::function<void()> task);

  // --- blocking work off home; the reply comes back on home ------------------------------------
  template <class R>
  void PostJob(WorkerLane lane,
               std::function<R(const CancelToken&)> work,   // captures values and services, never the owner
               std::function<void(R)> reply);               // runs on home; dropped once closing

  // --- handing out -------------------------------------------------------------------------------
  /** A callable for another thread or owner: it posts `fn` onto home, and is a no-op once closing. */
  template <class F> auto BindPost(F fn) const;
  /** An Executor whose posts this scope counts and drops (library executors: MeshExecutors, …). */
  std::shared_ptr<Executor> Executor(WorkerLane lane) const;
  WeakScope Weak() const;                  // copyable, any thread; Post no-ops once the scope closes
  template <class T> ScopedRef<T> Ref(T* self) const;  // a typed message handle (§ 2.6)

  // --- closing (on home) ---------------------------------------------------------------------------
  /** Step 1: refuse new work, cancel timers and the token; queued tasks and replies become no-ops. */
  void Close();
  /** Step 2: `on_closed` runs on `where` once no job is in flight (at once when none is). */
  void WhenClosed(std::shared_ptr<SerialExecutor> where, std::function<void()> on_closed);
  /** Close() then wait for the jobs, bounded. Lifecycle edges and tests only; false at the budget. */
  bool CloseAndWait(std::chrono::milliseconds budget);

  bool IsCurrent() const;                  // on home; used by PP_ASSERT_ON_SCOPE(scope_)
  size_t Pending() const;                  // tests, diagnostics, the shutdown_context mark
  const CancelToken& Token() const;
};

} // namespace pp
```

**How it is built.** `TaskScope` generalises `OwnerTasks`:

- A `pp::Lifetime` makes queued tasks no-ops, as in `OwnerTasks::Post` (`common:src/runtime/OwnerTasks.cpp:43-47`).
- A shared handle under a mutex makes late handle posts no-ops, as in `OwnerTasks::Close` (`:16-22`).
- New: an atomic pending count and job count, a cancel flag, `PostJob`, `WhenClosed`, and pooled sequences.

`OwnerTasks` becomes a `TaskScope` on an owner sequence (an alias during migration). `CallsLoop` and `BroadcastHub`
keep their shape.

**Pooled sequence.** It is a mutex-guarded deque plus a "scheduled" flag. A post to an idle sequence posts one
drain task to its pool lane. The drain runs up to N tasks (or about 2 ms), then re-posts itself, so one busy
sequence cannot hold a pool thread for good. `RunsTasksOnCurrentThread` is a thread-local "current sequence"
pointer that the drain sets. A pooled sequence never runs two tasks at once and never needs a thread of its own.
This is Chromium's `SequencedTaskRunner` over its thread pool.

### 2.3 Close, cancel, and what happens to a running task

A scope moves through three states: Open, then Closing, then Closed.

| Work | At `Close()` (on home) | Afterwards |
|------|------------------------|------------|
| Task queued on home | stays queued, runs as a no-op | — |
| Task running on home | it is the caller, or none: home is serial and `Close` runs on home | the caller finishes its task |
| Timer pending | cancelled | — |
| Job queued on a worker | runs as a no-op (it checks the scope before `work`) | its count settles |
| Job running on a worker | keeps running; `Token().Cancelled()` is now true | it finishes or stops early; its reply is dropped; its count settles |
| Post through a `WeakScope`, `ScopedRef`, `BindPost` or `Executor()` | refused (no-op) | — |
| `PostJob` or `Post` from inside a running job | refused | — |

When the job count reaches 0, the scope is Closed and `WhenClosed` callbacks run. **The owner may be destroyed
only after Closed.**

The rule that makes this safe without waiting: **a job's `work` never touches the owner.** It captures values, and
`shared_ptr`s of services that are thread-safe and outlive it (a relay client, a store). Only `reply` touches the
owner, and it runs on home or not at all. Two cases follow:

- An owner whose jobs follow that rule is Closed as soon as its jobs finish. Its members are not reachable from
  any job, so the job count matters for *resource release* (a file to delete, a socket to free), not for memory
  safety.
- An owner whose jobs reach into its members has to wait for them. That is **two-step close** (§ 2.4).

A job can end only as fast as the blocking call inside it. The token makes jobs end early where the call can be
interrupted: curl's progress callback returns non-zero, a loop checks between steps, and an Argon2 run checks
between passes. A job stuck in a call that cannot be interrupted keeps its count, and only *that* owner's close
waits (§ 3.2).

### 2.4 Blocking rules

1. **Never block on your own sequence.** `RunAndWait` onto your own home runs inline, as today
   (`AppRuntime.h:189`). Waiting from home for a job, or for a task that needs home, is a deadlock. Debug builds
   abort when `CloseAndWait` is called on home.
2. **Waits go downward only** (T004, unchanged): UI, then media sessions, then the mesh io lane. Upward is always a
   post.
3. **Owners with work in flight use two-step close.** The parent calls `child.Close()` and then
   `child.WhenClosed(parent_home, [&]{ child_.reset(); })`. The parent does not wait in its destructor. An owner
   with no jobs in flight may simply be destroyed: its scope's destructor closes it. A destructor that finds jobs
   still in flight is a bug: debug builds abort, and release builds wait at most `kDtorBudget` (100 ms), log, and
   leak.
4. **`CloseAndWait` is for lifecycle edges and tests,** where a caller below the owner's home waits (quit's last
   step, a test fixture). It never runs from a task of the scope it closes.
5. **Long blocking work runs as jobs** (network, disk beyond a few ms, crypto). A pooled home may do short
   blocking work (one SQLite write), never network, so its sequence stays responsive. The UI sequence and owner
   threads never block (THREADING rule 3, unchanged).

### 2.5 Timers

Scope timers are home-sequence timers. `PostAfter` and `PostEvery` are cancelled by `Close`, and a fired timer
whose scope is closing is a no-op. The coordinator stays the process timer wheel: `AppRuntime::ScheduleOn` already
works by firing a one-shot that posts onto an owner (`common:src/runtime/AppRuntime.cpp:780`). A scope timer is the
same thing aimed at the scope's weak handle, not at a mailbox. The hub's four repeating coordinator timers
(`ConversationsHub.cpp:2021-2037`) and its timer ids become `PostEvery` members. `StopCoordinatorTimers`
(`:2056`) becomes `Cancel` calls, or is dropped and left to `Close`.

### 2.6 Cross-owner messages and weak handles

```cpp
ScopedRef<ConversationsHub> hub = scope_.Ref(this);   // handed to a child, a GUI controller, a mesh hook
hub.Post(&ConversationsHub::OnRelayPinsChanged);      // runs on the hub's home, or not at all
hub.Post([](ConversationsHub& h) { h.NotifyOurRelaysChanged(); });
```

- A `ScopedRef<T>` is a `WeakScope` plus a `T*`. It dereferences the pointer only inside a task on T's home, after
  checking the scope, which is the same rule as Chromium's `WeakPtr`. It replaces three things: `OwnerTasks::Handle`
  plus a raw pointer (`CallsLoop::Ref`), raw `this` handed to callbacks (`SetCardFetcher`,
  `ConversationsHub.cpp:1321`; `SetSyncHandler`, `:2040`), and `Lifetime::Bind`, which runs inline on the calling
  thread (`Lifetime.h:70-78`).
- Messages carry values, and shared views are immutable snapshots (`shared_ptr<const T>`), which is THREADING
  owner rule 2, unchanged.
- **A request with a reply** is a message carrying a reply handle: `ScopedRef<Caller>` or `BindPost(callback)`. The
  reply lands on the caller's home, and nobody waits.
- Parent-only destroy is unchanged ([OWNERSHIP](../../docs/architecture/OWNERSHIP.md)). A child asks its parent
  to close it with a message; it never closes or frees itself.

### 2.7 The UI thread and the mesh pump

- **UI.** The UI mailbox becomes the UI sequence, with the same frame drain and wake path (`RunUITasks`,
  `RequestForceFrame`; THREADING § UI delivery pipeline). GUI controllers (`ChatController`, `ShellHost`,
  `Settings*`) and the hub (at first, see Q3) have UI as home. Plain `AppRuntime::PostUI` stays for the
  composition root and the runtime itself.
- **Mesh.** No pp-cpp-mesh change is needed. The mesh already follows the model: control runs on its io lane, it
  never calls the app synchronously, and it tells the app through events on that lane
  (`mesh:docs/THREADING.md` § Rules 1-4). The app's side changes in two places:
  - The handlers the app installs become `BindPost` or `ScopedRef` posts onto the app owner's home. They replace
    handlers that post raw `this`, for example `MeshDeliveryOrchestrator.cpp:142`.
  - The mesh's `work` and `blocking` executors (`capabilities/base/src/foundation/runtime/
    AppRuntimeMeshExecutors.cpp:9-13`, now `AppRuntime::WorkerExecutor`) come from the mesh owner's scope
    (`scope_.Executor(lane)` in `ProductMesh`). Mesh work still queued at mesh stop is then dropped with
    `ProductMesh`'s scope, not by the global gate.

  pp-cpp-mesh's and pp-cpp-amp's internal `pp::Lifetime` use (L4 coordinators, transports; OWNERSHIP.md:97-102)
  stays: it guards their io-lane state inside the library. The pump (`MeshPumpThread`, sole `Drive`) is unchanged.
- **Media threads and `AiTurnPool`.** These stay outside the general pool (THREADING.md:63-65). `AgentSessions`
  gets a scope for what its turns post back (L7), so a profile reset closes it like any other owner.

### 2.8 Blocking work, end to end (the hub's card reissue)

Today (`ConversationsHub.cpp:393-404`):

```cpp
AppRuntime::PostUI([this]() {                          // UI
  if (shutdown_requested_ || !IsMessagingReady()) return;
  if (checking_passes_ == 0 && SetCardRelays() && IsMainDevice())
    AppRuntime::PostWorkerNormal([this]() { ReissueAndAnnounceCard(); });   // worker, touches the hub
  NotifyOurRelaysChanged();
});
```

Target:

```cpp
scope_.Post([this]() {                                  // home; no flag: a closed scope does not run this
  if (!IsMessagingReady()) return;
  if (checking_passes_ == 0 && SetCardRelays() && IsMainDevice()) {
    scope_.PostJob<Roe<Card>>(WorkerLane::Normal,
        [identity = identity_sp_, clients = clients_sp_, relays = CardRelaysSnapshot()](const CancelToken& t) {
          return ReissueCard(*identity, *clients, relays, t);   // values and services only
        },
        [this](Roe<Card> card) { OnCardReissued(std::move(card)); });  // home
  }
  NotifyOurRelaysChanged();
});
```

`checking_passes_` stops being an atomic because it is read and written only on home.

---

## 3. Teardown, owner by owner

### 3.1 Quit

```text
RequestExit → HideWindow                                   (unchanged)
→ BeginShutdown (generation, 3 s deadline, watchdog)        (unchanged)
→ Application closes its children in reverse build order (each: Close on its home, via RunAndWait downward
  for owner-thread runners → WhenClosed → reset):
    GUI controllers → AgentSessions → product (hub subtree, calls / broadcast runners, ProductMesh)
  pumping UI and the manual owners until all are Closed or the 2 s budget ends
→ owners not Closed at the budget: FlushForExit on each, leak that owner and its ancestors (log names + Pending())
→ MeshHost::Stop / AppRuntime::Shutdown / RmlUi / Backend (unchanged)
```

Inside the hub, the order of `ConversationsHub::Shutdown` (`ConversationsHub.cpp:4140-…`) stays. It is the
dependency order of its members. What changes:

- each child (`MeshDeliveryOrchestrator`, `InboxController`, the workflows) closes its own scope first;
- the flag checks and the three `InvalidateAndWait` calls go away;
- what is left in a stuck state is one owner, not "all of messaging".

### 3.2 Profile reset and mid-life restart

`StopMessagingForReset` (`Application.cpp:673`) closes the hub subtree and `AgentSessions` only. The UI, calls
chrome and the runtime keep running, so no `ReopenAfterTeardown` epoch is needed. The reset waits for Closed with
its 10 s budget, because it must delete files the jobs may hold. "Try again" remains only when a job of *that
subtree* is stuck in a call that cannot be interrupted. That is rare: with cancel tokens, a curl job ends at its
next progress tick.

The same path destroys and recreates a subsystem mid-life (L4): a relay client on a settings change, a per-account
hub, the mesh on a network-settings rewire.

### 3.3 What remains of the global quiesce

- **A process-exit backstop.** `QuiesceForTeardown` still runs at quit, after the owners close. After T4 it should
  find nothing pending. It logs `teardown_quiesce_unscoped pending=N` when it does, which is the measure of posts
  still made outside a scope.
- **The allowlisted sites during migration (T1–T4).** The gate keeps covering them. Until T4 exits, profile reset
  still quiesces globally after closing the scoped owners.
- **After T5,** `ReopenAfterTeardown` and `DrainWorkersThenUI` leave product code, and the THREADING line "do not
  add per-owner gates for teardown" is superseded by TS001. `AppRuntime::Shutdown`'s budgeted joins, detach and
  leak (`common:src/runtime/ThreadRuntime.cpp:33-48`) and the 3 s watchdog stay. They cover the process, not
  memory safety.

---

## 4. Testing

- **Owners destroyed in tests are safe by construction.** A test builds an owner on a manual sequence. When the
  owner is destroyed, its scope's destructor closes the scope (the scope is the last member, so it goes first),
  so its queued tasks are no-ops in any later drain. The PR #676 crash
  (L1/L2) cannot happen. That PR's `TestHub` drain helper and the "post if running, else inline" fallbacks
  (`ConversationsHub.cpp:138`, `:3143`, `:3168`) go away: a test drains the manual sequence when it wants the work
  to run.
- **Debug checks:**
  - a scope destroyed with jobs in flight aborts, naming the scope and `Pending()`;
  - `PP_ASSERT_ON_SCOPE(scope_)` checks owner-state access;
  - in tests, `ExpectScopeIdle(scope)` checks that a test left nothing in flight.
- **TaskScope unit tests (pp-cpp-common):**
  - close with tasks queued, timers pending, a job queued and a job running (its reply is dropped and the token is
    set);
  - a post through a weak handle or `ScopedRef` after destroy;
  - `WhenClosed` ordering;
  - `CloseAndWait` on home refused in debug;
  - a pooled sequence keeps order, never runs two tasks at once, and is fair to other sequences;
  - TSan on all of the above.
- **Regressions per migrated owner:**
  - destroy with work in flight in every phase (queued, running job, timer armed);
  - destroy and recreate in one process with posts still in flight (two hubs in a row; this is the PR #676
    scenario as a gtest);
  - ASan with `detect_stack_use_after_return=1` over the whole binary, repeated.
- **Shutdown latency checks** (THREADING § Shutdown latency checks) are re-run after T2, T3 and T4 with the same
  pass bars.

---

## 5. Enforcement

`scripts/check/check_task_scopes.py`, run in CI's lint job next to `check_owner_runners.sh`
(`.github/workflows/build.yml:173`):

- **It rejects new unscoped posts.** That is any `AppRuntime::(PostUI|PostUIFront|PostWorker*|PostCoordinator*|
  Schedule*|PostTo|PostToFront|PostToOwnerOrRun)` call whose closure captures `this`, `&` or a raw owner pointer.
  It parses from the call to its matching parenthesis, so a capture list on the next line is seen. Tests and the
  runtime itself are exempt.
- **It also rejects** new `pp::Lifetime` members in app code outside the OWNERSHIP whitelist, and new
  `AppRuntime::IsRunning()` post-or-inline fallbacks.
- **The allowlist** is `scripts/check/task_scopes_allowlist.txt`, one line per file with today's count, in the same
  style as `capability_edges_allowlist.txt`. A new site fails. So does a count lower than the line: lower the line
  in the same PR. A file's line is deleted when it reaches 0.
- **Tightening.** At T5 the check rejects *any* direct `AppRuntime::Post*` or `Schedule*` in product code outside
  the composition root (`Application`, `ProductOwners`) and the runtime adapters. `check_owner_runners.sh` is
  updated to accept `TaskScope` in runners (its `FORBIDDEN` pattern lists `OwnerTasks`, `check_owner_runners.sh:
  33-37`).

---

## 6. Performance

- **Posting.** Today's gate takes one global mutex twice per task, at post and at run (`GateTask`,
  `common:src/runtime/AppRuntime.cpp:253-300`). A scope adds a weak-handle check and two atomic counter updates, so
  the global lock gives way to per-scope state. We expect it to be neutral or better under contention, and T0
  measures it.
- **Pooled sequences** add no threads. Their cost is one deque and one drain hop per batch. An owner that used to
  fan work out over 4 workers now serializes its own state changes. Parallel work stays parallel as jobs.
- **Memory** is one wrapper allocation per task, as `Lifetime::Bind` has today (`Lifetime.h:69`), plus one small
  block per scope.
- **UI latency** is unchanged: same queue, same frame drain.
- **Head-of-line blocking.** One busy pooled sequence holds at most one pool thread at a time, and the drain yields
  after N tasks or about 2 ms.
- **T0 benchmark:** post-and-run throughput and p99 latency (gate vs scope, 1 to 8 posting threads), and pooled
  sequence fairness with one sequence saturated. The bar: no regression above 10 % on the gate path.

---

## 7. Alternatives considered

| Option | For | Against | Verdict |
|--------|-----|---------|---------|
| **Status quo plus a test helper** (PR #676's `TestHub`, one per suite) | No product change; fixes the flaky suite now | L1–L7 stay. Each suite must remember its helper. Product safety still rests on order and budgets. No mid-life restart. | Take #676 now as a test fix; it does not replace this project |
| **One `pp::Lifetime` per owner, binding all its posts** | Mechanical; the type exists; no new runtime | `Bind` runs on whichever thread calls, so owner code stays multi-threaded and keeps its locks (L6). `InvalidateAndWait` in a destructor waits for bound calls on other threads, and deadlocks if one waits on the destroying thread. No count of off-thread jobs, so it cannot tell when a resource is free. It is a third mechanism. | Rejected as the model; TaskScope uses `Lifetime` inside |
| **`shared_ptr` / `enable_shared_from_this` everywhere** (posts capture `weak_from_this`, or pin) | Familiar; no use-after-free by refcount | Owners live until their last task, so destruction runs on any thread at any time, against parent-only destroy and destroy-on-owner ([OWNERSHIP](../../docs/architecture/OWNERSHIP.md)). Teardown order becomes non-deterministic (SQLite files still open when a reset wipes them). Cycles. Owner code is still multi-threaded. | Rejected. Allowed only for *services* a job borrows (§ 2.3), with a delete-on-sequence deleter where a service has affinity |
| **A full actor framework** (CAF), or **`std::execution`** (P2300; `counting_scope` / `async_scope`, P3149) | A principled model; structured concurrency; stop tokens built in | C++26: not in our toolchains (g++ 14, Apple clang, MSVC, NDK). stdexec would be a large header-only dependency for every pp-cpp-* library. It is a new programming model across the tree. THREADING prefers explicit steps to coroutines (§ Owner runners, multi-step flows). | Rejected now. TaskScope keeps its ideas (`counting_scope` = pending count plus join, `stop_token` = CancelToken, nursery = scope), so it can map onto `std::execution` later |

Prior art: Chromium's `SequencedTaskRunner`, with `WeakPtrFactory` invalidated on the owning sequence and
`DeleteSoon` / `RefCountedDeleteOnSequence`; C++26 `std::execution::counting_scope`; Swift task groups and actors;
Kotlin `CoroutineScope` (cancelled with its owner; children are joined).

---

## 8. Risks

| Risk | What happens | Mitigation |
|------|--------------|------------|
| **A destructor deadlocks** | A destructor waits for jobs that need the destroying thread, or waits upward | Two-step close is the rule. Destructors never wait in debug builds (they abort if jobs are in flight). `CloseAndWait` is banned on home. A debug watchdog logs any scope wait over 100 ms with the scope's name. Waits stay downward-only (T004). |
| **Ordering changes** | Work that ran on any worker now runs serially on home. A task now lands on a different queue than its old neighbours (it used to be FIFO behind a UI post from another owner). | Migrate in two passes per owner (PHASES T2a/T2b). Pass a binds to the scope on the *same* thread as today (lifetime only). Pass b moves state to home and drops locks. Any ordering an owner relied on across owners is named and kept explicit (`PostFront` with a comment, as today). |
| **Migration bugs** | A reply posted where the old code ran inline; a job capturing `this`; a dropped reply that a waiter needed ("arm ⇒ complete on cancel", THREADING § Cancel / Abort) | Each `PostJob` reply that completes a waiter gets an explicit close path (Close completes it with a "closed" result). The check flags `this` inside `PostJob` work lambdas. A per-owner destroy-in-every-phase test (§ 4). |
| **Two models during migration** | Scoped and unscoped owners coexist for several releases | The global gate stays as the backstop until T4 exits. The allowlist only counts down. |
| **Pool starvation** | Several busy pooled sequences plus blocking jobs on 2–4 threads | Pooled homes do short work only (§ 2.4 rule 5). Drains yield. Jobs keep their lane priorities. T0 tests fairness. |
| **A library version bump** | `TaskScope` lands in pp-cpp-common, which pp-ledger also uses | Additive (minor version). `OwnerTasks` and `Lifetime` keep their API. |
