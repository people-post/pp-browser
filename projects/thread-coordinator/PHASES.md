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

- [ ] Add `WorkerPool` in `src/base/platform/` (or `src/common/`)
  - Fixed size 2–4 (constant for now)
  - Three queues or single queue with lane bias (Critical dequeued first)
  - `Post(lane, fn)` and `PostAndReply(lane, work, on_done)` on coordinator or UI thread
  - Join on shutdown; drop queued work when stopped (match IO thread Stop semantics)
- [ ] Unit tests: priority ordering, pool size cap, shutdown does not hang
- [ ] Logging: queue depth per lane (debug)

**Exit criteria:** Pool usable from tests; no production call sites yet.

---

## Phase t2 — Migrate libp2p hop-offs to pool

**Goal:** Remove detached threads from libp2p integration services.

- [ ] `ReachabilityService` — UPnP / dial-back probe → pool Background
- [ ] `DialBackService` — stream handler RPC → pool Normal or Critical
- [ ] `CircuitRelayService` — bridge copy loop → pool Normal
- [ ] `MediaRelayService` — relay streams → pool Normal
- [ ] `CallMediaDirectService` — direct media streams → pool Normal
- [ ] libp2p fork `cares.cpp` — DNS TXT query → pool Background (result still posted to `io_context`)
- [ ] Delete all `.detach()` in `src/libp2p/integration/host/`

**Exit criteria:** Zero detach in libp2p integration; libp2p IO handlers remain non-blocking; reachability/call tests pass.

---

## Phase t3 — Migrate messaging / call hop-offs to pool

**Goal:** Replace feature-layer detached threads; preserve Critical semantics.

- [ ] `P2pMessagingService` — PollInbox loop → pool Background with coalescing atomics preserved
- [ ] `P2pMessagingService` — prefer_relay send, AckInbox → pool Normal
- [ ] `MessagingHub` — N025 / mDNS workers → pool Critical where signaling
- [ ] `Libp2pDirectChatService`, `Libp2pChatHistoryService` — stream hop-offs → pool Normal
- [ ] `CallLifecycle` — AcceptInvite → pool **Critical**
- [ ] `CallLibp2pMediaBridge` — connect workers → pool Critical / Normal
- [ ] Delete all `.detach()` in `src/feature/messaging/`

**Exit criteria:** Zero detach in messaging/calls; AcceptInvite not starved by PollInbox (manual or automated soak).

---

## Phase t4 — CoordinatorThread + timer wheel

**Goal:** Single orchestration mailbox; move periodic policy off UI tick.

- [ ] Add `CoordinatorThread` with mailbox (`CoordinatorMessage`: source, priority, fn)
- [ ] Timer wheel: schedule relay poll (2s / 45s), peer idle sweep (~15s), deferred retries
- [ ] Wire sources:
  - UI intents → coordinator (thin posts from controllers / ports)
  - libp2p stream events → coordinator (via existing service callbacks)
  - `PushWakeJni` / `RequestWakeSync` → coordinator immediate message
  - Linux notifier activations → coordinator
  - Worker pool completions → coordinator
- [ ] Move policy from `MessagingHub::TickLibp2p` into coordinator handlers (UI tick calls thin `Coordinator::PumpTimers` or coordinator owns wall clock)
- [ ] Remove or gut `BackgroundSyncScheduler::Tick` from UI frame path

**Exit criteria:** Relay poll and peer sweep run from coordinator timer wheel; UI frame no longer drives sync policy.

---

## Phase t5 — Merge BrowserThread::IO into coordinator + pool

**Goal:** Retire dedicated `pp-browser-io` thread; keep `BrowserThread` API stable.

- [ ] Route `BrowserThread::PostTask(IO, …)` → coordinator Normal or pool Normal
- [ ] Route `PostTaskFront(IO, …)` → pool Critical
- [ ] Route `PostTaskAndReply` → pool + UI reply (unchanged UI side)
- [ ] Map `PauseIO` / `ResumeIO` to coordinator + pool pause
- [ ] Migrate `AgentSession`, `HttpClient` call sites — verify LLM/tools on pool Normal
- [ ] Remove `SequencedTaskRunner` dedicated IO thread; coordinator thread runs mailbox loop
- [ ] Update [RUNTIME_COMPOSITION.md](../../docs/architecture/RUNTIME_COMPOSITION.md) diagram to target topology

**Exit criteria:** No `pp-browser-io` thread; ~97 `PostTask` call sites behave equivalently; app lifecycle pause/resume works on Android background.

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
| 2026-08-03 | Project created; t0 design complete; t1–t6 planned |
