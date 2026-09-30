# Node metrics

**Tier:** contract

What pp-node serves at `GET /metrics` on its status HTTP server (`--status-addr`, default `127.0.0.1:18518`; the `--status-token` bearer applies): the Prometheus text exposition format 0.0.4. Scrape it with Prometheus or an OpenTelemetry collector's Prometheus receiver. Scrape only, no push; the registry and exposition are hand-rolled (`common/metrics/MetricsRegistry.*`, no Prometheus / OpenTelemetry SDK). Rationale: [node-monitoring D001](../../projects/node-monitoring/DECISIONS.md#d001--scrape-only-hand-rolled).

## Rules

- Names: `pp_<area>_<name>[_<unit>]`; counters end in `_total`; units are base units (`_seconds`, `_bytes`).
- Labels are low-cardinality and fixed per series. **Never** peer ids, account ids, addresses, call / session ids or content — the same rule as the `Metrics` log channel (`common/Metrics.h`).
- Series without labels, and series whose label set is small and fixed (e.g. `direction`, `result`, `protocol`), exist from process start at 0 where their component exists. Series labelled per event (`pp_link_*` by `path` / `direction` / `reason` / `stage`) appear with their first event: use `or vector(0)` / `absent()` in queries. Names are stable — renaming one is a contract change.

## Series

| Name | Type | Labels | Meaning |
|------|------|--------|---------|
| `pp_build_info` | gauge | `version` | Always 1 |
| `pp_node_capability` | gauge | `service` = `circuit_relay` \| `media_relay` \| `dht` | 1 while serving |
| `pp_process_start_time_seconds` | gauge | | Unix seconds |
| `pp_process_uptime_seconds` | gauge | | |
| `pp_process_cpu_seconds` | gauge | | User + system CPU, monotonic (use `rate()`); Linux |
| `pp_process_resident_memory_bytes` | gauge | | Linux |
| `pp_process_threads` | gauge | | Linux |
| `pp_process_open_fds` | gauge | | Linux |
| `pp_runtime_queue_depth` | gauge | `owner` | Owner-thread tasks queued, not started |
| `pp_runtime_task_wait_seconds` | histogram | `owner` | Queued → started |
| `pp_runtime_task_run_seconds` | histogram | `owner` | Started → done |
| `pp_media_relay_sessions` | gauge | | Hosted sessions with participants |
| `pp_media_relay_participants` | gauge | | Across those sessions |
| `pp_media_relay_frames_total` | counter | `direction` = `received` \| `forwarded` | Data frames (forwarded counts each recipient) |
| `pp_media_relay_bytes_total` | counter | `direction` | Data frame bytes |
| `pp_media_relay_dropped_frames_total` | counter | `reason` = `stale` \| `not_carried` | Stale latest-lossy frames; a channel not agreed for the sender — video of another level, or a reserved kind ([MEDIA_CHANNELS](MEDIA_CHANNELS.md)) |
| `pp_media_relay_quotes_total` | counter | `result` = `issued` \| `refused_admission` \| `refused_video_level` \| `refused_busy` | |
| `pp_media_relay_attaches_total` | counter | `result` = `ok` \| `refused` | |
| `pp_amp_datagrams_total` | counter | `direction` = `sent` \| `received` | Amp UDP datagrams |
| `pp_amp_bytes_total` | counter | `direction` | Amp UDP datagram bytes |
| `pp_amp_datagrams_rejected_total` | counter | | Received datagrams no association took (bad HMAC / decode) |
| `pp_amp_reliable_packets_total` | counter | `event` = `sent` \| `retransmitted` \| `lost` | `lost` = given up after the retry cap |
| `pp_amp_rtt_seconds` | histogram | | Round trips: acks of never-retransmitted reliable packets (buckets 0.005 … 2.5) |
| `pp_link_active` | gauge | | Amp links in the link table |
| `pp_amp_channels_open` | gauge | `protocol` = `directory` \| `dht` \| `reach` \| `punch` \| `circuit` \| `circuit_carrier` \| `rpc_chat` \| `rpc_history` \| `rpc_peer_announce` \| `rpc_broadcast` \| `blob` \| `realtime` \| `datagram_relay` \| `other` | Open L3 channels across all links. Fixed label set: ids outside the shipped map ([L4_PROTOCOL_KINDS.md](L4_PROTOCOL_KINDS.md)) count as `other` |
| `pp_link_connects_total` | counter | `path` = `direct` \| `punched` \| `carrier`; `direction` = `outbound` \| `inbound` | Links that connected |
| `pp_link_drops_total` | counter | `reason` ([AMP-LINK-ERRORS](AMP-LINK-ERRORS.md) drop reasons, e.g. `connection-dead`, `handshake-timeout`); `stage` = `connected` \| `attempt` | `attempt` = never connected (a failed dial) |
| `pp_link_path_changes_total` | counter | | Remote endpoint migrations |
| `pp_reachability_status` | gauge | `status` = `unknown` \| `checking` \| `reachable` \| `outbound_only` \| `blocked` | 1 for the current verdict |
| `pp_reachability_signal` | gauge | `signal` = `dial_back_ok` \| `seed_dial_ok` \| `upnp_mapped` \| `public_ipv4` \| `global_ipv6` | 1 = true |
| `pp_punch_attempts_total` | counter | `kind` = `cold` \| `upgrade` \| `signaling`; `result` = `ok` \| `failed` | Punches this node started |
| `pp_punch_served_total` | counter | `role` = `introducer` \| `target` | Punch requests served |
| `pp_circuit_relay_requests_total` | counter | `op` = `bridge` \| `reserve`; `result` = `accepted` \| `refused_admission` \| `refused_standby_full` (bridge) / `accepted` \| `refused` (reserve) | |
| `pp_circuit_relay_tunnels_total` | counter | `result` = `bridged` \| `failed` | Tunnels that ended setup |
| `pp_circuit_relay_setup_seconds` | histogram | | Request → bridged (buckets 0.05 … 10) |
| `pp_circuit_relay_tunnels` | gauge | `state` = `bridged` \| `setup` | Open now |
| `pp_circuit_relay_reservations` | gauge | | Answerers parked here |
| `pp_circuit_relay_bytes_total` | counter | | Bytes spliced through bridges, both directions |
| `pp_circuit_parked_relays` | gauge | | Relays holding a reservation for this node (client side: rendezvous parking) |
| `pp_dht_records` | gauge | | Records cached |
| `pp_dht_inbound_requests_total` | counter | `op` = `find_peer` \| `store` | Served |
| `pp_dht_inbound_rate_limited_total` | counter | | Refused by the per-peer limit |
| `pp_dht_store_rejected_total` | counter | | |
| `pp_dht_lookups_total` | counter | | `find_peer` lookups issued |

Owner names: `pp-media-sess`, `pp-connectivity`. Histogram buckets (seconds): 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1, 5.

Code: `common/metrics/MetricsRegistry.*` (registry + rendering), `app/node/NodeMetrics.*` (scrape-time collectors).
