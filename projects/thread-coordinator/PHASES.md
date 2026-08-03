# Phased roadmap — thread coordinator

**Short-lived file.** Delete [`projects/thread-coordinator/`](.) when all phases below are shipped and [THREADING.md](../../docs/architecture/THREADING.md) reflects live code.

Can proceed **in parallel** with [p2p-av-calls m2](../p2p-av-calls/PHASES.md) (WebRTC teardown reduces hidden pool pressure). No hard dependency on chat-storage or e2e.

---

## Phase t0 — Design (done)

**Goal:** Agree fixed-role model and document migration.

- [x] Inventory current threads ([THREADING.md § Today](../../docs/architecture/THREADING.md#today-2026-08))
- [x] Target topology: UI + libp2p + coordinator + worker pool + timer wheel
- [x] Phased plan (this file)

**Exit criteria:** [THREADING.md](../../docs/architecture/THREADING.md) published; project folder created.

---

## Phase t1 — WorkerPool skeleton

**Goal:** Bounded pool with Critical / Normal / Background lanes; test harness only.

- [x] Add `WorkerPool` in `src/common/` (`WorkerPool.h`, `WorkerPool.cpp`)
  - Fixed size 2–4 (clamped at init)
  - Three lane queues (Critical dequeued first)
  - `Post(lane, fn)` and `PostAndReply(lane, work, on_done)` (`on_done` runs on pool thread)
  - `Pause` / `Resume` / `Shutdown` (drop queued on shutdown; join workers)
- [x] Unit tests: priority ordering, pause, shutdown, clamp — `src/common/tests/worker_pool_test.cpp`
- [ ] Logging: queue depth per lane (debug) — deferred
- [ ] Production call sites — phase t2+

**Exit criteria:** Pool usable from tests; no production call sites yet. **Done (except debug queue metrics).**

---

## Phase t2 — Migrate libp2p hop-offs to pool

**Goal:** Remove detached threads from libp2p integration services.

- [x] `Libp2pHost` owns `WorkerPool`; shutdown before io thread join
- [x] `ReachabilityService` — UPnP / dial-back probe → pool Background
- [x] `DialBackService` — stream handler RPC → pool Normal
- [x] `CircuitRelayService` — bridge copy loop + RPC → pool Normal
- [x] `MediaRelayService` — relay streams / quote / attach → pool Normal
- [x] `CallMediaDirectService` — direct media connect/inbound → pool Critical
- [ ] libp2p fork `cares.cpp` — DNS TXT query → pool Background (**deferred:** fork must not link `pp_common`; keep detach until libp2p executor hook)
- [x] Delete all `.detach()` in `src/libp2p/integration/host/`

**Exit criteria:** Zero detach in libp2p integration; libp2p IO handlers remain non-blocking. **Done** (cares deferred).

---

## Phase t3.5 — PlatformRuntime (app-owned pool)

**Goal:** Composition root owns worker pool; call sites use `PlatformRuntime::PostWorker` (no `WorkerPool&` plumbing).

- [x] `ThreadRuntime` owns `WorkerPool` (internal to platform)
- [x] `WorkerDispatch` in `common/` — installed by `PlatformRuntime::Initialize`
- [x] `PlatformRuntime` — `PostUI`, `PostWorker`, `PostWorkerAndReplyOnUI`, `EnsurePlatformServices`, `Paths()` / `Assets()` / `Notifier()`
- [x] `Application` / `pp-node` call `PlatformRuntime::Initialize` / `Shutdown`
- [x] libp2p integration uses `PostLibp2pWorker` (dispatch when installed; private host pool in unit tests)
- [ ] Wire `AppLifecycle` background → `PlatformRuntime::PauseWorkers()` — deferred
- [ ] Register libp2p io / notifier / mDNS on runtime — deferred (t4+)

**Exit criteria:** Single schedule API in production; no `SetWorkerPool` / `WorkerPool*` through NodeRuntime. **Done** (pause/lifecycle deferred).

---

## Phase t3 — Migrate messaging / call hop-offs to pool

**Goal:** Replace feature-layer detached threads; preserve Critical semantics.

- [x] `P2pMessagingService` — PollInbox loop → pool Background with coalescing atomics preserved
- [x] `P2pMessagingService` — prefer_relay send, AckInbox → pool Critical / Normal
- [x] `MessagingHub` — N025 / mDNS workers → pool Critical
- [x] `Libp2pDirectChatService`, `Libp2pChatHistoryService` — stream hop-offs → pool Normal
- [x] `CallLifecycle` — AcceptInvite → pool **Critical**
- [x] `CallLibp2pMediaBridge` — connect workers → Critical; MediaKey poll → Background
- [x] Delete all `.detach()` in `src/feature/messaging/`

**Exit criteria:** Zero detach in messaging/calls; AcceptInvite not starved by PollInbox (manual or automated soak). **Done** (soak deferred).

---

## Phase t4 — CoordinatorThread + timer wheel

**Goal:** Single orchestration mailbox; move periodic policy off UI tick.

- [x] Add `CoordinatorThread` with mailbox (`CoordinatorPriority`: Critical / Normal / Background)
- [x] Timer wheel: `ScheduleRepeating` / `ScheduleOneShot` / `CancelTimer`
- [x] Expose via `PlatformRuntime`: `PostCoordinator`, `ScheduleCoordinatorRepeating`, etc.
- [x] `ThreadRuntime` starts/stops coordinator with worker pool
- [x] Relay poll (2s / 45s) on coordinator via `BackgroundSyncScheduler` (wake → `PostCoordinatorCritical`)
- [x] `MessagingHub` hub policy (peer sweep, mDNS, reachability) on 1s coordinator timer
- [x] Remove `BackgroundSyncScheduler::Tick` from UI frame; remove `MessagingHub::TickLibp2p` from `Application::Run`
- [ ] Wire all sources (notifier activations, worker completions → coordinator) — partial; push wake done
- [ ] Linux notifier activations → coordinator — deferred

**Exit criteria:** Relay poll and peer sweep run from coordinator timer wheel; UI frame no longer drives sync policy. **Done** (notifier → coordinator deferred).

---

## Phase t5 — Merge BrowserThread::IO into coordinator + pool

**Goal:** Retire dedicated `pp-browser-io` thread; keep `BrowserThread` API stable.

- [x] Route `BrowserThread::PostTask(IO, …)` → worker pool Normal
- [x] Route `PostTaskFront(IO, …)` → pool Critical
- [x] Route `PostTaskAndReply` / `PostTaskFrontAndReply` → pool + UI reply
- [x] Map `PauseIO` / `ResumeIO` → `PlatformRuntime::PauseBackgroundWork` (coordinator + pool)
- [x] Remove dedicated IO `SequencedTaskRunner` thread (`SequencedTaskRunner` is UI-only)
- [x] Add `SequencedTaskRunner.cpp` to `pp_common` (was missing from CMake)
- [x] Unit tests: `BrowserThreadTest` IO routing + pause/resume
- [x] Update [RUNTIME_COMPOSITION.md](../../docs/architecture/RUNTIME_COMPOSITION.md) diagram
- [ ] Linux notifier activations → coordinator — deferred from t4
- [ ] Full Android background soak — deferred

**Exit criteria:** No `pp-browser-io` thread; IO call sites route through pool; app lifecycle pause/resume works. **Done** (soak deferred).

---

## Phase t6 — Cleanup and archive

**Goal:** Delete transitional code and this project folder.

- [ ] Grep `src/` for remaining `.detach()` — only documented third-party exceptions
- [ ] Update [THREADING.md](../../docs/architecture/THREADING.md): collapse “Today” into brief history or remove
- [ ] Update [AGENTS.md](../../AGENTS.md) common tasks row for threading
- [ ] Remove [projects/thread-coordinator/](.) entirely
- [ ] Optional: metrics dashboard hook (queue depth / lane wait time)

**Exit criteria:** Project folder gone; architecture doc describes only live model.

---

## Changelog

| Date | Change |
|------|--------|
| 2026-08-03 | Phase t5: retire `pp-browser-io`; `BrowserThread::IO` → worker pool |
| 2026-08-03 | Phase t4: `CoordinatorThread` + timer wheel; relay poll + hub policy off UI tick |
| 2026-08-03 | Phase t3: messaging hop-offs on `PlatformRuntime::PostWorker` |
| 2026-08-03 | Phase t2: libp2p integration hop-offs on `WorkerPool` |
| 2026-08-03 | Phase t1: `WorkerPool` in `src/common/` with unit tests |
| 2026-08-03 | Project created; t0 design complete; t1–t6 planned |
