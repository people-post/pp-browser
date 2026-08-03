# Current state — threading

**As of 2026-08-03.** Normative target: [THREADING.md](../../docs/architecture/THREADING.md).

## Implemented

| Component | Location | Notes |
|-----------|----------|-------|
| UI sequenced runner | `BrowserThread::UI`, `SequencedTaskRunner(false)` | Inline drain on main |
| App IO thread | `BrowserThread::IO`, `SequencedTaskRunner(true)` | Named `pp-browser-io` on Linux/Android |
| **PlatformRuntime (t3.5)** | `src/base/platform/PlatformRuntime.*` | Unified facade: `PostWorker`, `PostUI`, `PostCoordinator`, timer APIs |
| **Worker pool (t1–t3)** | `WorkerPool` via `PlatformRuntime` | libp2p + messaging hop-offs; **0** `.detach()` in integration + messaging |
| **Coordinator (t4)** | `CoordinatorThread` via `PlatformRuntime` | Priority mailbox + timer wheel; relay poll + hub policy |
| libp2p reactor | `Libp2pHost::io_thread_` | asio `io_context::run()`; borrows app pool (private pool in unit tests) |
| Relay poll | `BackgroundSyncScheduler` | 2s / 45s on coordinator timer; wake → `PostCoordinatorCritical` |
| Hub periodic policy | `MessagingHub` | 1s coordinator timer: peer sweep, mDNS, reachability UX |
| UI wake | `Backend::WakeEventLoop`, `BrowserThread::SetUIWakeCallback` | SDL user event |
| IO pause/resume | `BrowserThread::PauseIO` / `ResumeIO` | AppLifecycle, AgentSession, BackgroundSyncScheduler |
| Hop-off threads | **0** `.detach()` in `feature/messaging/` (t3) | libp2p fork `cares.cpp` still detached |
| Linux notifier watch | `LocalNotifier_Linux.cpp` | Dedicated joinable thread (not yet posting to coordinator) |

## Not started

- Linux notifier activations → coordinator mailbox
- Worker pool completions → coordinator (optional orchestration)
- Deprecation of `BrowserThread::IO` dedicated thread (t5)

## Next agent — start here

1. Read [THREADING.md](../../docs/architecture/THREADING.md) target section.
2. Pick [PHASES.md § Phase t5](PHASES.md#phase-t5--merge-browserthreadio-into-coordinator--pool).
3. Do not remove Browser IO until t5; coordinator + pool are live in production via `ThreadRuntime`.
