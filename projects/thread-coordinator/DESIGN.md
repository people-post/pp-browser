# Design — thread coordinator

## Scope

Replace ad hoc threading with fixed roles:

1. UI thread (unchanged)
2. libp2p reactor (unchanged)
3. **Coordinator** — mailbox + timer wheel + policy (new)
4. **Worker pool** — 2–4 threads, Critical / Normal / Background lanes (new)
5. Platform I/O — Linux D-Bus notifier pattern retained

Out of scope for this project:

- RmlUi render threading
- libp2p fork internal scheduler redesign
- SQLite schema / store API changes (mutex discipline stays)
- WebRTC/libdatachannel removal (parallel [p2p-av-calls](../p2p-av-calls/) track)

## Canonical spec

Full target architecture, affinity table, and API mapping: **[docs/architecture/THREADING.md](../../docs/architecture/THREADING.md)**.

## Key types (planned)

Location TBD under `src/base/platform/`:

```
CoordinatorThread   — owns std::thread, mailbox, timer wheel
WorkerPool          — fixed N threads, 3 dequeues or 1 heap with lane bias
CoordinatorMessage  — source, priority, deadline, std::function<void()>
BrowserThread       — façade; IO posts become coordinator/pool routes
```

## Non-goals

- `std::async` (banned in libp2p fork; not introduced in app)
- Unbounded thread-per-task
- Running LLM inference on UI thread

## Risks

| Risk | Mitigation |
|------|------------|
| Regress AcceptInvite / N025 starvation | Critical lane + phase 4 soak tests; keep PostTaskFront semantics until pool proven |
| Background Android wake | Timer wheel must fire when UI idle; push wake posts to coordinator directly |
| Shutdown hang | Pool cancel tokens; abandon queued work like today's IO Stop() |
| Phase 2 coordinator too heavy | Code review rule: no curl/UPnP in coordinator handlers |
