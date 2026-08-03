# Current state — threading

**As of 2026-08-03.** Normative target: [THREADING.md](../../docs/architecture/THREADING.md).

## Implemented

| Component | Location | Notes |
|-----------|----------|-------|
| UI sequenced runner | `BrowserThread::UI`, `SequencedTaskRunner(false)` | Inline drain on main |
| App IO thread | `BrowserThread::IO`, `SequencedTaskRunner(true)` | Named `pp-browser-io` on Linux/Android |
| **ThreadRuntime (t3.5)** | `src/base/platform/ThreadRuntime.*` | App / pp-node owns `WorkerPool`; `PauseWorkers` / `ResumeWorkers` |
| **Worker pool (t1–t2)** | `WorkerPool` via `ThreadRuntime` | libp2p integration hop-offs; **0** `.detach()` in `libp2p/integration/host/` |
| libp2p reactor | `Libp2pHost::io_thread_` | asio `io_context::run()`; borrows app pool (private pool in unit tests) |
| UI wake | `Backend::WakeEventLoop`, `BrowserThread::SetUIWakeCallback` | SDL user event |
| IO pause/resume | `BrowserThread::PauseIO` / `ResumeIO` | AppLifecycle, AgentSession, BackgroundSyncScheduler |
| Hop-off threads | ~13× `.detach()` in `feature/messaging/` | See THREADING.md today section |
| UI-tick polling | `BackgroundSyncScheduler::Tick`, `MessagingHub::TickLibp2p` | Not yet on coordinator timer wheel |
| Linux notifier watch | `LocalNotifier_Linux.cpp` | Dedicated joinable thread |

## Not started

- `CoordinatorThread` / mailbox / timer wheel
- Migration of messaging/call hop-offs to shared pool (t3)
- Coordinator-owned relay poll / peer idle scheduling
- Deprecation of `BrowserThread::IO` dedicated thread (merged into coordinator + pool)

## Next agent — start here

1. Read [THREADING.md](../../docs/architecture/THREADING.md) target section.
2. Pick [PHASES.md § Phase t3](PHASES.md#phase-t3--migrate-messaging--call-hop-offs-to-pool).
3. Do not remove Browser IO until t5; app pool is live in production via `ThreadRuntime`.
