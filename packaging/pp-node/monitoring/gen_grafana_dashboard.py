#!/usr/bin/env python3
"""Generate grafana-dashboard.json (pp-node fleet) from the NODE_METRICS.md series.

Usage: python3 packaging/pp-node/monitoring/gen_grafana_dashboard.py > packaging/pp-node/monitoring/grafana-dashboard.json
"""
import json
import sys

DS = {"type": "prometheus", "uid": "${datasource}"}
SEL = 'deployment_environment=~"$env",host_name=~"$host"'

panels = []
_id = [0]
_y = [0]


def nid():
    _id[0] += 1
    return _id[0]


def row(title):
    panels.append({"type": "row", "title": title, "id": nid(), "collapsed": False,
                   "gridPos": {"h": 1, "w": 24, "x": 0, "y": _y[0]}, "panels": []})
    _y[0] += 1


_x = [0]


def place(w, h):
    if _x[0] + w > 24:
        _x[0] = 0
        _y[0] += h
    pos = {"h": h, "w": w, "x": _x[0], "y": _y[0]}
    _x[0] += w
    return pos


def end_row(h):
    _x[0] = 0
    _y[0] += h


def ts(title, targets, unit="short", w=8, h=8, desc="", stack=False):
    panels.append({
        "type": "timeseries", "title": title, "id": nid(), "datasource": DS, "description": desc,
        "gridPos": place(w, h),
        "fieldConfig": {"defaults": {"unit": unit, "custom": {
            "fillOpacity": 10, "showPoints": "never",
            "stacking": {"mode": "normal" if stack else "none"}}}, "overrides": []},
        "options": {"legend": {"displayMode": "list", "placement": "bottom"},
                    "tooltip": {"mode": "multi", "sort": "desc"}},
        "targets": [{"datasource": DS, "expr": e, "legendFormat": l, "refId": chr(65 + i)}
                    for i, (e, l) in enumerate(targets)],
    })


def stat(title, expr, legend, unit="short", w=6, h=4, desc="", mappings=None):
    panels.append({
        "type": "stat", "title": title, "id": nid(), "datasource": DS, "description": desc,
        "gridPos": place(w, h),
        "fieldConfig": {"defaults": {"unit": unit, "mappings": mappings or []}, "overrides": []},
        "options": {"reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
                    "colorMode": "value", "graphMode": "none", "textMode": "value_and_name"},
        "targets": [{"datasource": DS, "expr": expr, "legendFormat": legend, "refId": "A",
                     "instant": True}],
    })


H = "{{host_name}}"

row("Overview")
stat("Version", f"max by (host_name, version) (pp_build_info{{{SEL}}})", H + " {{version}}",
     desc="pp_build_info: one per node, labelled with its version.")
stat("Uptime", f"pp_process_uptime_seconds{{{SEL}}}", H, unit="s")
stat("Reachability", f"max by (host_name, status) (pp_reachability_status{{{SEL}}} == 1)",
     H + " {{status}}", desc="The node's current reachability verdict (pp_reachability_status == 1).")
stat("Serving", f"max by (host_name, service) (pp_node_capability{{{SEL}}} == 1)",
     H + " {{service}}", desc="Capabilities the node is serving (circuit_relay, media_relay, dht).")
end_row(4)

row("Process")
ts("pp-node memory (RSS)", [(f"pp_process_resident_memory_bytes{{{SEL}}}", H)], unit="bytes",
   desc="The pp-node process only. Normal is a few MiB to tens of MiB.")
ts("pp-node CPU", [(f"rate(pp_process_cpu_seconds{{{SEL}}}[$__rate_interval])", H)],
   unit="percentunit", desc="User + system CPU, fraction of one core.")
ts("Threads / open fds", [(f"pp_process_threads{{{SEL}}}", H + " threads"),
                          (f"pp_process_open_fds{{{SEL}}}", H + " fds")])
end_row(8)
ts("Machine memory used", [(f'system_memory_usage_bytes{{{SEL},state="used"}}', H + " used"),
                           (f"sum by (host_name) (system_memory_usage_bytes{{{SEL}}})", H + " total")],
   unit="bytes", w=12,
   desc="Whole machine (hostmetrics). 'used' excludes page cache; 'total' is all states summed = installed RAM.")
ts("Container memory", [(f'container_memory_usage_total_bytes{{{SEL},container_name="pp-node"}}', H)],
   unit="bytes", w=12,
   desc="docker_stats for the pp-node container. Counts page cache, so it reads higher than RSS.")
end_row(8)

row("Mesh")
ts("Links", [(f"pp_link_active{{{SEL}}}", H)])
ts("Open channels by protocol",
   [(f"sum by (host_name, protocol) (pp_amp_channels_open{{{SEL}}})", H + " {{protocol}}")], stack=True)
ts("Link connects / drops",
   [(f"sum by (host_name, path) (rate(pp_link_connects_total{{{SEL}}}[$__rate_interval]))",
     H + " connect {{path}}"),
    (f"sum by (host_name, reason) (rate(pp_link_drops_total{{{SEL}}}[$__rate_interval]))",
     H + " drop {{reason}}")], unit="ops")
end_row(8)

row("Traffic")
ts("Amp bytes", [(f"sum by (host_name, direction) (rate(pp_amp_bytes_total{{{SEL}}}[$__rate_interval]))",
                  H + " {{direction}}")], unit="Bps")
ts("Reliable packets: retransmitted / lost",
   [(f'sum by (host_name, event) (rate(pp_amp_reliable_packets_total{{{SEL},event=~"retransmitted|lost"}}[$__rate_interval]))',
     H + " {{event}}"),
    (f"sum by (host_name) (rate(pp_amp_datagrams_rejected_total{{{SEL}}}[$__rate_interval]))",
     H + " rejected datagrams")], unit="pps")
ts("RTT p50 / p95",
   [(f"histogram_quantile(0.5, sum by (host_name, le) (rate(pp_amp_rtt_seconds_bucket{{{SEL}}}[$__rate_interval])))",
     H + " p50"),
    (f"histogram_quantile(0.95, sum by (host_name, le) (rate(pp_amp_rtt_seconds_bucket{{{SEL}}}[$__rate_interval])))",
     H + " p95")], unit="s")
end_row(8)

row("Relays")
ts("Media relay", [(f"pp_media_relay_sessions{{{SEL}}}", H + " sessions"),
                   (f"pp_media_relay_participants{{{SEL}}}", H + " participants")])
ts("Media relay frames",
   [(f"sum by (host_name, direction) (rate(pp_media_relay_frames_total{{{SEL}}}[$__rate_interval]))",
     H + " {{direction}}"),
    (f"sum by (host_name, reason) (rate(pp_media_relay_dropped_frames_total{{{SEL}}}[$__rate_interval]))",
     H + " dropped {{reason}}")], unit="ops")
ts("Circuit relay", [(f"sum by (host_name, state) (pp_circuit_relay_tunnels{{{SEL}}})", H + " tunnels {{state}}"),
                     (f"pp_circuit_relay_reservations{{{SEL}}}", H + " reservations")])
end_row(8)
ts("Relay bytes",
   [(f"sum by (host_name) (rate(pp_media_relay_bytes_total{{{SEL}}}[$__rate_interval]))", H + " media"),
    (f"rate(pp_circuit_relay_bytes_total{{{SEL}}}[$__rate_interval])", H + " circuit")], unit="Bps", w=12)
ts("Relay requests",
   [(f"sum by (host_name, result) (rate(pp_media_relay_quotes_total{{{SEL}}}[$__rate_interval]))",
     H + " quote {{result}}"),
    (f"sum by (host_name, op, result) (rate(pp_circuit_relay_requests_total{{{SEL}}}[$__rate_interval]))",
     H + " circuit {{op}} {{result}}")], unit="ops", w=12)
end_row(8)

row("Runtime")
ts("Owner-thread queue depth", [(f"pp_runtime_queue_depth{{{SEL}}}", H + " {{owner}}")])
ts("Task wait p95",
   [(f"histogram_quantile(0.95, sum by (host_name, owner, le) (rate(pp_runtime_task_wait_seconds_bucket{{{SEL}}}[$__rate_interval])))",
     H + " {{owner}}")], unit="s")
ts("DHT", [(f"pp_dht_records{{{SEL}}}", H + " records"),
           (f"sum by (host_name) (rate(pp_dht_inbound_requests_total{{{SEL}}}[$__rate_interval]))", H + " requests/s")])
end_row(8)

dashboard = {
    "title": "pp-node fleet",
    "uid": "pp-node-fleet",
    "description": "pp-node /metrics (docs/contracts/NODE_METRICS.md) via the OpenTelemetry Collector; "
                   "host_name / deployment_environment labels come from the collector.",
    "tags": ["pp-node"],
    "timezone": "browser",
    "schemaVersion": 39,
    "version": 1,
    "refresh": "1m",
    "time": {"from": "now-6h", "to": "now"},
    "templating": {"list": [
        {"name": "datasource", "label": "Data source", "type": "datasource", "query": "prometheus",
         "current": {}, "hide": 0},
        {"name": "env", "label": "Environment", "type": "query", "datasource": DS,
         "query": {"query": "label_values(pp_build_info, deployment_environment)", "refId": "env"},
         "definition": "label_values(pp_build_info, deployment_environment)",
         "includeAll": True, "multi": True, "allValue": ".*", "refresh": 2, "current": {}, "hide": 0},
        {"name": "host", "label": "Node", "type": "query", "datasource": DS,
         "query": {"query": 'label_values(pp_build_info{deployment_environment=~"$env"}, host_name)', "refId": "host"},
         "definition": 'label_values(pp_build_info{deployment_environment=~"$env"}, host_name)',
         "includeAll": True, "multi": True, "allValue": ".*", "refresh": 2, "current": {}, "hide": 0},
    ]},
    "panels": panels,
}
json.dump(dashboard, sys.stdout, indent=2)
sys.stdout.write("\n")
