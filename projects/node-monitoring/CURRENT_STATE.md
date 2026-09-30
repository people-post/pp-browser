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

**M2 landed** (links, reachability, punches, circuit relay): `MeshLinkEventLog.cpp` (`InstallMeshLinkMetrics`), `AmpPunchCoordinator.h`, `PunchServer.cpp`, `CircuitRelayServer.cpp` (`RuntimeStats`), `NodeMetrics.cpp`.

**Next:** stats that need pp-cpp-amp (per-link RTT / loss / bytes, channels by protocol, circuit bridge bytes); M3 (pp-browser UI snapshot, with rendezvous parking).
