# Task scopes: phases

This file gives the order of work only. Status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Each phase is one or
a few PRs, and each leaves main shippable, with the global quiesce still in place as the backstop until T5. The
whole project waits for TS001 to be accepted, and starts after release 1. T0 and T1 change no runtime behaviour and
could land earlier ([Q9](DECISIONS.md#open-questions-for-hongwei)).

## T0 — TaskScope in the runtime, with tests

Where: pp-cpp-common `common/runtime/` (Q1), as a minor version. pp-browser then bumps its pin
(`cmake/PpCppCommon.cmake:19`).

- [ ] `TaskScope`, `WeakScope`, `ScopedRef<T>` and `CancelToken` ([DESIGN § 2.2](DESIGN.md#22-api-sketch-pp-cpp-common-commonruntimetaskscopeh)).
- [ ] Sequences: `UISequence`, `OwnerSequence`, `NewPooledSequence`, and manual sequences for tests.
- [ ] `PostJob`, `PostAfter` / `PostEvery`, `BindPost`, `Executor(lane)`, `Close` / `WhenClosed` / `CloseAndWait`.
- [ ] `PP_ASSERT_ON_SCOPE`, and the debug abort for a scope destroyed with jobs in flight.
- [ ] `OwnerTasks` re-expressed over `TaskScope` (same API, same tests pass).
- [ ] Unit tests from [DESIGN § 4](DESIGN.md#4-testing), under TSan; the benchmark from [DESIGN § 6](DESIGN.md#6-performance).

**Exit:**

- the tests pass on Linux, macOS and Windows, and under TSan on Linux;
- `runtime_owner_executor_test` and `app_runtime_teardown_test` pass unchanged;
- the gate path regresses by 10 % or less in the benchmark;
- pp-browser builds on the new pin, with no product user yet.

## T1 — The check, with an allowlist

- [ ] `scripts/check/check_task_scopes.py` ([DESIGN § 5](DESIGN.md#5-enforcement)): it parses multi-line calls,
      exempts tests and the runtime, and also flags new `pp::Lifetime` members outside the OWNERSHIP whitelist
      and new `IsRunning()` post-or-inline fallbacks.
- [ ] `scripts/check/task_scopes_allowlist.txt`, holding today's exact per-file counts. The first-line grep finds
      246 sites in 33 files ([DESIGN § 1.1](DESIGN.md#11-the-model)); the parser's numbers replace that.
- [ ] Wired into CI's lint job next to `check_owner_runners.sh`.
- [ ] One paragraph in THREADING.md: new posts go through a scope, and the old mailboxes are legacy.

**Exit:**

- CI fails a PR that adds a raw-`this` post or lets a count drift without lowering its line;
- the counts are recorded in CURRENT_STATE;
- `scripts/README.md` lists the check.

## T2 — `ConversationsHub` and its subtree

Owners: `ConversationsHub` (45 sites), `MeshDeliveryOrchestrator` (48), `InboxController`, `PushDeviceCoordinator`,
`GroupMembershipWorkflow`, `GroupInviteLinkWorkflow`, `AttachmentFetchWorkflow` (messaging), and `DeliveryOutbox` /
`ControlLane` (delivery).

**T2a (lifetime: same threads as today)**

- [ ] Each owner gets a `TaskScope`. Every raw-`this` post goes through it. A worker body stays on a worker, as a
      scope-counted post on the worker lane (`scope_.Executor(lane)`), so no thread changes yet.
- [ ] The hub's three `pp::Lifetime`s (`ConversationsHub.h:901`, `:996`, `:1014`) and their `InvalidateAndWait`
      calls (`ConversationsHub.cpp:4151-4159`) are replaced by the scope.
- [ ] Coordinator timers become `PostEvery` (`ConversationsHub.cpp:2021-2037`). `BackgroundSyncScheduler`'s
      handler (`:2040`) and the card fetcher (`:1321`) take a `ScopedRef`.
- [ ] `RequestShutdown` becomes `Close` for the subtree. `Shutdown` runs after `WhenClosed`.
- [ ] Profile reset closes the subtree first, then still runs the global quiesce (backstop).

**T2b (affinity: state on its home)**

- [ ] The hub's home is UI (Q3). Worker bodies that touch hub state become `PostJob`s, whose work captures values
      and services and whose reply lands on home ([DESIGN § 2.8](DESIGN.md#28-blocking-work-end-to-end-the-hubs-card-reissue)).
- [ ] `shutdown_requested_` checks inside posted bodies go. Locks and atomics that guarded state now confined to
      home go.
- [ ] Tests drop the `IsRunning()` fallbacks and PR #676's `TestHub` drain.

**Exit:**

- the allowlist has no line for the files above;
- `hub_participant_test` passes 200/200 under ASan (`detect_stack_use_after_return=1`) without `TestHub`, together
  with the whole binary;
- a new test destroys a hub with work in flight (queued post, running job, armed timer) and then builds a second
  hub in the same process;
- the hub's mutex and atomic count is reported in the PR (18 and 7 before);
- the shutdown latency checks pass (THREADING § Shutdown latency checks);
- quit with a stuck relay job leaks only the hub subtree (log shows the scope).

## T3 — `CallStack`, calls and broadcast

- [ ] `CallsLoop` and `BroadcastHub` hold a `TaskScope` on the media-sessions sequence (this replaces the
      `OwnerTasks` alias). `CallsLoop::Ref` becomes a `ScopedRef`.
- [ ] `CallStack::Shutdown` stops using the process-wide `DrainWorkersThenUI` (`CallStack.cpp:795`). It waits for
      its own scope and for delivery's control-lane jobs of this call (delivery's scope from T2), downward and
      bounded.
- [ ] `chrome_self_` (`CallStack.h:316`, `CallStack.cpp:752`) and the UI edge's posts (`CallUiBackend.cpp:27`,
      `:63`, `:86`) go through scopes.
- [ ] `check_owner_runners.sh` accepts `TaskScope` in runners (its `FORBIDDEN` pattern, `:33-37`).

**Exit:**

- no `DrainWorkersThenUI` in product code;
- calls and broadcast gtests pass, and the lab `smoke` calls scenarios pass;
- quit mid-ring and quit mid-connect pass (THREADING § Shutdown latency checks);
- the calls tree's files are off the allowlist.

## T4 — The rest

- [ ] GUI controllers with UI as home: `ChatController` (32), `ShellHost` (11), `SettingsSecurityActions` (11),
      `CallController` (10), the other `Settings*`, `AgentTurnController`, `ChatMenus`, `EmojiPickerController`,
      `AvatarCropController`, `ContactsController`, `ClientCompatController`, `CallPresenter`,
      `AndroidCallSystemUi`.
- [ ] Composition: `Application` (13), `ProductMesh` (its mesh `work` / `blocking` executors come from its scope:
      [DESIGN § 2.7](DESIGN.md#27-the-ui-thread-and-the-mesh-pump)), `StorageManagerBridge`, `ConfigApplyBridge`,
      `BackgroundSyncScheduler`, `ThreadCompactionService`.
- [ ] `AgentSessions` gets a scope for what its turns post back.
- [ ] Quit closes the owners in reverse build order ([DESIGN § 3.1](DESIGN.md#31-quit)). The closing global quiesce
      logs `teardown_quiesce_unscoped`.
- [ ] Profile reset closes the profile's owners only, with no `ReopenAfterTeardown`.

**Exit:**

- the allowlist is empty, or lists residuals each with a reason;
- across the lab `smoke` set and the shutdown latency checks, `teardown_quiesce_unscoped` never reports pending
  work;
- profile reset never says "try again" in the lab, apart from a deliberately stuck job;
- a mid-life restart test (relay client rebuilt on a settings change) passes with traffic in flight.

## T5 — Docs, and the quiesce demoted to a backstop

- [ ] THREADING.md is rewritten: § Task scopes replaces § Teardown quiesce and the Scheduling API table, the
      "do not add per-owner gates for teardown" line goes, and § Owner runners is expressed in scopes.
      OWNERSHIP.md's § Lifetime and its whitelist are narrowed to the mesh libraries.
- [ ] `QuiesceForTeardown` is called only at process exit. `ReopenAfterTeardown` and `DrainWorkersThenUI` leave
      product code (they are kept, or deprecated, in pp-cpp-common for other hosts).
- [ ] The check is tightened: no direct `AppRuntime::Post*` / `Schedule*` in product code outside the composition
      root and the runtime adapters.
- [ ] TS001 is marked "superseded by THREADING.md § Task scopes". The project moves to Done in
      [projects/README.md](../README.md).

**Exit:**

- the docs describe one mechanism;
- CI enforces it;
- `projects/thread-ownership`'s T002 ("behind the teardown gate") is annotated as amended by TS001.
