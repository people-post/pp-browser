# Current state — threading

**As of 2026-08-03.** Normative target: [THREADING.md](../../docs/architecture/THREADING.md).

## Implemented

| Component | Location | Notes |
|-----------|----------|-------|
| UI sequenced runner | `BrowserThread::UI`, `SequencedTaskRunner(false)` | Inline drain on main |
| App IO thread | `BrowserThread::IO`, `SequencedTaskRunner(true)` | Named `pp-browser-io` on Linux/Android |
| libp2p reactor | `Libp2pHost::io_thread_` | asio `io_context::run()` |
| UI wake | `Backend::WakeEventLoop`, `BrowserThread::SetUIWakeCallback` | SDL user event |
| IO pause/resume | `BrowserThread::PauseIO` / `ResumeIO` | AppLifecycle, AgentSession, BackgroundSyncScheduler |
| Hop-off threads | 25× `.detach()` in 13 files | See THREADING.md today section |
| UI-tick polling | `BackgroundSyncScheduler::Tick`, `MessagingHub::TickLibp2p` | Not yet on coordinator timer wheel |
| Linux notifier watch | `LocalNotifier_Linux.cpp` | Dedicated joinable thread |

## Not started

- `CoordinatorThread` / mailbox / timer wheel
- `WorkerPool` with priority lanes
- Migration of hop-offs to pool
- Coordinator-owned relay poll / peer idle scheduling
- Deprecation of `BrowserThread::IO` dedicated thread (merged into coordinator + pool)

## Next agent — start here

1. Read [THREADING.md](../../docs/architecture/THREADING.md) target section.
2. Pick [PHASES.md § Phase t1](PHASES.md#phase-t1--workerpool-skeleton).
3. Do not remove hop-offs until pool Critical lane is proven in t2.
