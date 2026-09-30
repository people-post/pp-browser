# Node monitoring — current state

**As of:** 2026-09-30
**Branch:** `refactor/mesh-connectivity`

**Done.** M1–M3 shipped; stable refs: [NODE_METRICS.md](../../docs/contracts/NODE_METRICS.md) (series), [WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md) (popover), [CALLS.md](../../docs/architecture/CALLS.md) § Call media health (Call details).

**M1 landed.** `GET /metrics` on the pp-node status server (bearer auth), Prometheus text; series in [NODE_METRICS.md](../../docs/contracts/NODE_METRICS.md).

| Piece | Path |
|-------|------|
| Registry + rendering | `common/metrics/MetricsRegistry.*` |
| Endpoint | `app/node/StatusHttpProtocol.cpp` |
| Node collectors (process, capabilities, relay load, DHT) | `app/node/NodeMetrics.*`, `foundation/platform/os/OsProcessStats_*` |
| Owner-thread queues | `foundation/runtime/OwnerThread.*`, `AppRuntime.cpp` |
| Media relay counters | `MediaRelayServer.cpp` |

**M2 landed** (links, reachability, punches, circuit relay): `MeshLinkEventLog.cpp` (`InstallMeshLinkMetrics`), `AmpPunchCoordinator.h`, `PunchServer.cpp`, `CircuitRelayServer.cpp` (`RuntimeStats`), `NodeMetrics.cpp`.

**Amp traffic landed** (pp-cpp-amp v2.10.0: `Endpoint::Stats`, RTT observer, `ChannelBridge::ForwardedBytes`): `pp_amp_*` series, `pp_circuit_relay_bytes_total`.

**M3 landed (popover):** `MeshTrafficTotals` / `MeshTrafficRatesBetween` / `CollectMeshTrafficTotals` in `feature/conversations/MessagingShellPorts.*`; rendered by `ShellHost::ApplyStatusbarPopover` into `window_shell.rml`.

Rendezvous parking: `CircuitClientCoordinator::ParkedRelayCount` → `pp_circuit_parked_relays` and the popover's network section.

Open channels by protocol (pp-cpp-amp v2.11.0): `pp_amp_channels_open{protocol}`, peer-chosen ids outside the shipped map fold into `other`.

Call quality (pp-cpp-amp v2.12.0 per-connection stats): `CallLinkCounters` / `CallLinkHealthBetween` in `common/media/CallMediaHealth.*`, `CallMediaLegCoordinator::ActiveLinkCounters`, `CallHopHealth::link`; shown in Call details and the `media_health` log.
