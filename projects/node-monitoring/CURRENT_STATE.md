# Node monitoring — current state

**As of:** 2026-09-30
**Branch:** `refactor/mesh-connectivity`

**M1 landed.** `GET /metrics` on the pp-node status server (bearer auth), Prometheus text; series in [NODE_METRICS.md](../../docs/contracts/NODE_METRICS.md).

| Piece | Path |
|-------|------|
| Registry + rendering | `common/metrics/MetricsRegistry.*` |
| Endpoint | `app/node/StatusHttpProtocol.cpp` |
| Node collectors (process, capabilities, relay load, DHT) | `app/node/NodeMetrics.*`, `foundation/platform/os/OsProcessStats_*` |
| Owner-thread queues | `foundation/runtime/OwnerThread.*`, `AppRuntime.cpp` |
| Media relay counters | `MediaRelayServer.cpp` |

**Next:** M2 (links, reachability, circuit relay).
