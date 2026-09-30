# Node monitoring — decisions

## D001 — Scrape only, hand-rolled

**Date:** 2026-09-30
**Decision:** Operators scrape a Prometheus-format `/metrics` on the pp-node status server; no push. The registry and exposition are our own (no opentelemetry-cpp / prometheus-cpp dependency).
**Rationale:** Monitored nodes are reachable (operators behind NAT are not a target). An OpenTelemetry collector scrapes the same endpoint, so OTel stacks work without an SDK; the SDKs would add protobuf / gRPC / abseil to every platform build.
