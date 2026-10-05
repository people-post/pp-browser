# pp-node monitoring

`grafana-dashboard.json` — a Grafana dashboard for a pp-node fleet, built on the
`/metrics` series in [NODE_METRICS.md](../../../docs/contracts/NODE_METRICS.md).
Regenerate it after changing series:

```bash
python3 packaging/pp-node/monitoring/gen_grafana_dashboard.py > packaging/pp-node/monitoring/grafana-dashboard.json
```

## Expected pipeline

pp-node serves Prometheus text at `GET /metrics` (status port, default 18518);
an OpenTelemetry Collector on the same host scrapes it and remote-writes to a
Prometheus-compatible store (our deployment: `nfsc-brief-web3/infra-ansible`
`roles/otelcol` → Amazon Managed Prometheus). The dashboard expects the labels
that collector adds:

| Label | Meaning |
|-------|---------|
| `host_name` | Node name (`qa-1`, `prod-1`, …) — the **Node** variable |
| `deployment_environment` | `qa` / `prod` — the **Environment** variable |

A plain Prometheus scrape has neither: add them as target labels in the scrape
config (`labels: {host_name: …, deployment_environment: …}`), or the variables
stay empty.

## Import

Grafana → Dashboards → New → Import → upload `grafana-dashboard.json`, then pick
the Prometheus data source (for AMP: an *Amazon Managed Service for Prometheus*
data source with SigV4 auth).

## Reading memory

| Panel | Series | What it measures |
|-------|--------|------------------|
| pp-node memory (RSS) | `pp_process_resident_memory_bytes` | The pp-node process — normally a few MiB to tens of MiB |
| Container memory | `container_memory_usage_total_bytes{container_name="pp-node"}` | The container, page cache included |
| Machine memory used | `system_memory_usage_bytes{state="used"}` | The whole machine; summing every `state` gives installed RAM |
