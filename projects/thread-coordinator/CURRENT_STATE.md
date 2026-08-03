# Current state — threading

**As of 2026-08-03.** Normative target: [THREADING.md](../../docs/architecture/THREADING.md).

## Implemented

| Component | Location | Notes |
|-----------|----------|-------|
| UI sequenced runner | `BrowserThread::UI`, `SequencedTaskRunner` | Inline drain on main |
| **PlatformRuntime** | `src/base/platform/PlatformRuntime.*` | `PostWorker`, `PostCoordinator`, `PauseBackgroundWork`, timer APIs |
| **Worker pool (t1–t5)** | `WorkerPool` via `PlatformRuntime` | All blocking work including legacy `BrowserThread::IO` posts |
| **Coordinator (t4–t5)** | `CoordinatorThread` via `PlatformRuntime` | Timer wheel + mailbox; pause/resume with pool |
| libp2p reactor | `Libp2pHost::io_thread_` | asio `io_context::run()` |
| Relay poll | `BackgroundSyncScheduler` | Coordinator timer; wake → `PostCoordinatorCritical` |
| Hub periodic policy | `MessagingHub` | 1s coordinator timer |
| IO pause/resume | `BrowserThread::PauseIO` / `ResumeIO` | Maps to coordinator + pool pause |
| Hop-off threads | **0** `.detach()` in integration + messaging | libp2p fork `cares.cpp` still detached |
| Linux notifier watch | `LocalNotifier_Linux.cpp` | Dedicated joinable thread |

## Retired

- `pp-browser-io` dedicated thread (`SequencedTaskRunner(true)`)
- UI-frame-driven sync policy (`BackgroundSyncScheduler::Tick`, `MessagingHub::TickLibp2p` in Run loop)

## Not started (t6)

- Grep cleanup for remaining `.detach()`
- Archive `projects/thread-coordinator/`
- Linux notifier → coordinator mailbox

## Next agent — start here

1. Read [THREADING.md](../../docs/architecture/THREADING.md).
2. Pick [PHASES.md § Phase t6](PHASES.md#phase-t6--cleanup-and-archive).
