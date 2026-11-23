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
Prometheus-compatible store (for example an Ansible-managed `otelcol` role writing to Amazon Managed Prometheus). The dashboard expects the labels
that collector adds:

| Label | Meaning |
|-------|---------|
| `host_name` | Node name (`prod-1`, `staging-2`, …) — the **Node** variable |
| `deployment_environment` | `prod` / `staging` / `main` (dev) — the **Environment** variable |

A plain Prometheus scrape has neither: add them as target labels in the scrape
config (`labels: {host_name: …, deployment_environment: …}`), or the variables
stay empty.

## Import

Grafana → Dashboards → New → Import → upload `grafana-dashboard.json`, then pick
the Prometheus data source (for AMP: an *Amazon Managed Service for Prometheus*
data source with SigV4 auth).

## Provisioning

A Grafana that provisions the dashboard from files generates its own copy with its values, from the generator at the
pp-node release it runs (`pp-node/vX.Y.Z`), so the dashboard matches the series that release serves:

```bash
python3 packaging/pp-node/monitoring/gen_grafana_dashboard.py \
  --datasource <uid> [--datasource <uid> ...] \
  --env Production=prod --env Staging=staging --env Main=main \
  --uid <uid> --title "<title>" --tag <tag> --read-only > nodes.json
```

| Option | Effect |
|--------|--------|
| `--datasource UID` | A fixed Prometheus data source in place of the picker. Given twice or more, every query runs on each (Grafana's *Mixed* data source): for a fleet whose environments write to different stores. The node filter is then a regex box, and the counts and the node table add the stores up |
| `--env LABEL=VALUE` | Fixed environment choices in place of the values read from `pp_build_info` |
| `--uid`, `--title`, `--tag` | The dashboard's identity in that Grafana |
| `--read-only` | Not editable in Grafana |

CI checks that the committed `grafana-dashboard.json` is the generator's default output.

## Reading the fleet

| Panel | Series | What it says |
|-------|--------|--------------|
| Nodes up | `pp_build_info` | Nodes whose `/metrics` reached the store in the last few minutes |
| Nodes down | `pp_build_info` over 24 h | Nodes seen in the last day whose `/metrics` no longer arrive: the node stopped, or its collector did. Red when not 0 |
| Directory heartbeat failing | `pp_node_heartbeat_last_success_timestamp_seconds` | Nodes with the heartbeat on (opt-in, org seeds) whose last success is over 10 min old. Not liveness: a node with the heartbeat off is not counted |
| Not reachable | `pp_reachability_status` | Nodes whose verdict is `blocked` or `outbound_only`. Yellow when not 0 |
| Nodes (table) | `pp_build_info`, `pp_process_uptime_seconds`, `pp_reachability_status`, `pp_node_capability` | One row per node: environment, version, uptime, reachability, and ✓ / – per service. Sort or filter by any column |

## Reading memory

| Panel | Series | What it measures |
|-------|--------|------------------|
| pp-node memory (RSS) | `pp_process_resident_memory_bytes` | The pp-node process — normally a few MiB to tens of MiB |
| Container memory | `container_memory_usage_total_bytes{container_name="pp-node"}` | The container, page cache included |
| Machine memory used | `system_memory_usage_bytes{state="used"}` | The whole machine; summing every `state` gives installed RAM |
