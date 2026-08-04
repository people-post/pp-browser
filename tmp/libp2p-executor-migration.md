# Libp2p executor migration (temporary tracker — delete when done)

Three executor classes: **Control** (app WorkerPool), **Data** (host io_context), **Compute** (optional service pool, future).

## Phase checklist

| Phase | Item | Status |
|-------|------|--------|
| **0** | `Libp2pExecutorConfig` | done |
| **0** | `Libp2pScheduler` facade | done |
| **0** | `StreamFrameIo` sync + async primitives | done |
| **0** | `Libp2pHost::IoExecutor()` accessor | done |
| **0** | `NodeRuntime` owns scheduler | done |
| **0** | `stream_frame_io_test` | done |
| **0** | THREADING.md executor section | done |
| **1** | `circuit_relay_service_test` | done |
| **1** | Async `StreamBridge` (legacy worker pumps removed) | done |
| **1** | Wire `SetExecutorConfig` in hub/node | done |
| **2** | DialBack dedupe onto `StreamFrameIo` / `StreamJsonFrame` | done |
| **2** | `BlockingReadStreamJson` / `BlockingWriteStreamJson` helpers | done |
| **2** | Concurrent dial-back test | done |
| **3** | Media relay structural split | done |
| **3** | Media relay async + tests | done |
| **3** | `DuplexFrameSession` per-stream serializer | done |
| **3** | Wire `SetExecutorConfig` in hub/node | done |
| **4** | Call-media uses shared primitives | pending |
| **5** | Direct chat services | pending |
| **6** | Compute stub + limits + flag removal | pending |

## PR log

| PR | Branch | Notes |
|----|--------|-------|
| 1 | `cursor/libp2p-executor-migration-771d` | Phase 0–2 + Phase 3 media relay async |

## Notes

- Circuit relay **data plane** now always uses host-io `StreamBridge` (legacy worker pumps removed — they deadlocked on Yamux).
- `async_data_plane_circuit_relay` flag retained for config compat; bridge path ignores false.

## Flags (`Libp2pExecutorConfig`)

- `async_data_plane_circuit_relay` — default **true** (legacy path removed)
- `async_data_plane_media_relay` — default **false**; hop inbound uses `DuplexFrameSession` on host io when true
- `async_data_plane_dial_back` — default false (control-only; no long-lived pump)
