# Node metrics

**Tier:** contract

What pp-node serves at `GET /metrics` on its status HTTP server (`--status-addr`, default `127.0.0.1:18518`; the `--status-token` bearer applies): the Prometheus text exposition format 0.0.4. Scrape it with Prometheus or an OpenTelemetry collector's Prometheus receiver. Design and phases: [projects/node-monitoring](../../projects/node-monitoring/).

## Rules

- Names: `pp_<area>_<name>[_<unit>]`; counters end in `_total`; units are base units (`_seconds`, `_bytes`).
- Labels are low-cardinality and fixed per series. **Never** peer ids, account ids, addresses, call / session ids or content — the same rule as the `Metrics` log channel (`common/Metrics.h`).
- A series exists from process start (at 0) where its component exists; names are stable — renaming one is a contract change.

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
| `pp_media_relay_dropped_frames_total` | counter | `reason` = `stale` \| `video_level` | Stale latest-lossy frames; video of a level not agreed for the sender ([MEDIA_CHANNELS](MEDIA_CHANNELS.md)) |
| `pp_media_relay_quotes_total` | counter | `result` = `issued` \| `refused_admission` \| `refused_video_level` \| `refused_busy` | |
| `pp_media_relay_attaches_total` | counter | `result` = `ok` \| `refused` | |
| `pp_dht_records` | gauge | | Records cached |
| `pp_dht_inbound_requests_total` | counter | `op` = `find_peer` \| `store` | Served |
| `pp_dht_inbound_rate_limited_total` | counter | | Refused by the per-peer limit |
| `pp_dht_store_rejected_total` | counter | | |
| `pp_dht_lookups_total` | counter | | `find_peer` lookups issued |

Owner names: `pp-media-sess`, `pp-connectivity`. Histogram buckets (seconds): 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1, 5.

Code: `common/metrics/MetricsRegistry.*` (registry + rendering), `app/node/NodeMetrics.*` (scrape-time collectors).
