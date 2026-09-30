# Node monitoring — design

## Goal

Two readers of one instrumentation layer:

1. **pp-node operators** scrape `/metrics` (Prometheus text format) on the node's status HTTP server — with Prometheus, or an OpenTelemetry collector's Prometheus receiver.
2. **pp-browser** shows its own figures in the UI (call quality, network, what a desktop Node serves) from a published snapshot — nothing leaves the device.

## Shape

- **Registry** (`common/metrics`): counters, gauges and fixed-bucket histograms with atomic updates; labels fixed at registration (low cardinality). Components only increment — no threads, no timers (THREADING.md § Owner runners). Gauges that already exist as snapshots (DHT stats, relay load, process) are read at scrape time by collectors, which must be thread-safe.
- **Exposition**: the registry renders Prometheus text format 0.0.4; `/metrics` serves it behind the status server's bearer auth.
- **Names**: [NODE_METRICS.md](../../docs/contracts/NODE_METRICS.md) — `pp_<area>_<name>_<unit>`, `_total` for counters.
- **Privacy**: the `Metrics.h` rule holds — no peer ids, account ids, addresses or content in names or labels.

## Not in scope

Push (OTLP) — nodes that operators monitor are reachable ([D001](DECISIONS.md#d001--scrape-only-hand-rolled)). Tracing. Per-peer or per-session series.
