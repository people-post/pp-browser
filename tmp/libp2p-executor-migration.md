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
| **6** | `stream_frame_io_test` | done |
| **0** | THREADING.md executor section | done |
| **1** | `circuit_relay_service_test` (legacy) | pending |
| **1** | Async `StreamBridge` + flag | pending |
| **1** | Circuit default async + remove legacy | pending |
| **2** | DialBack dedupe / optional io inbound | pending |
| **3** | Media relay structural split | pending |
| **3** | Media relay async + tests | pending |
| **4** | Call-media uses shared primitives | pending |
| **5** | Direct chat services | pending |
| **6** | Compute stub + limits + flag removal | pending |

## PR log

| PR | Branch | Notes |
|----|--------|-------|
| 1 | `cursor/libp2p-executor-migration-771d` | Phase 0 foundation — landed |

## Flags (`Libp2pExecutorConfig`)

- `async_data_plane_circuit_relay` — default false until phase 1 validated
- `async_data_plane_media_relay` — default false until phase 3 validated
- `async_data_plane_dial_back` — default false until phase 2 validated
