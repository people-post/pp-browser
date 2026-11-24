#!/usr/bin/env python3
"""Generate grafana-dashboard.json (pp-node fleet) from the NODE_METRICS.md series.

Usage: python3 packaging/pp-node/monitoring/gen_grafana_dashboard.py > packaging/pp-node/monitoring/grafana-dashboard.json

The committed file uses the defaults: a data source variable, environments and nodes read from the series. A deployment
that provisions the dashboard passes its own values instead (README § Provisioning):

  --datasource UID     a fixed Prometheus data source; twice or more: every query runs on each (Grafana's Mixed
                       data source), for a fleet whose environments write to different stores
  --env LABEL=VALUE    a fixed environment choice (repeatable), in place of the values read from the series
  --uid, --title       the dashboard's uid and title
  --tag TAG            an extra tag (repeatable)
  --read-only          not editable in Grafana (a provisioned dashboard)
"""
import argparse
import json
import sys

# Every query's data source, replaced in finish() by the mode the arguments ask for.
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


def stat(title, expr, legend, unit="short", w=6, h=4, desc="", mappings=None, bad=None):
    panels.append({
        "type": "stat", "title": title, "id": nid(), "datasource": DS, "description": desc,
        "gridPos": place(w, h),
        # bad: a count that should be 0 shows in this colour once it is not.
        "fieldConfig": {"defaults": {"unit": unit, "mappings": mappings or [], "color": {"mode": "thresholds"},
                                     "thresholds": {"mode": "absolute", "steps": (
                                         [{"color": "green", "value": None}, {"color": bad, "value": 1}] if bad
                                         else [{"color": "text", "value": None}])}},
                        "overrides": []},
        # A total: with several data sources (Mixed) each returns its own count, summed here.
        "transformations": [{"id": "seriesToRows", "options": {}}],
        "options": {"reduceOptions": {"calcs": ["sum"], "fields": "/^Value$/", "values": False},
                    "colorMode": "value", "graphMode": "none", "textMode": "value"},
        "targets": [{"datasource": DS, "expr": expr, "legendFormat": legend, "refId": "A",
                     "instant": True}],
    })


def table(title, expr, w=12, h=8, desc="", rename=None):
    panels.append({
        "type": "table", "title": title, "id": nid(), "datasource": DS, "description": desc,
        "gridPos": place(w, h),
        "fieldConfig": {"defaults": {}, "overrides": []},
        "options": {"showHeader": True},
        "transformations": [{"id": "labelsToFields", "options": {}}, {"id": "merge", "options": {}},
                            {"id": "organize", "options": {"excludeByName": {"Time": True}, "renameByName": rename or {}}}],
        "targets": [{"datasource": DS, "expr": expr, "legendFormat": "", "refId": "A", "instant": True,
                     "format": "table"}],
    })


def node_table(title, columns, w=24, h=14, desc=""):
    """One row per node, a column per query.

    `columns`: (key, expr, header, overrides). `expr` gives one number per `host_name`; each query is tagged with
    `col=<key>` and carries the node's environment and version (joined from pp_build_info), and Grafana's "join by
    labels" makes the rows. A query that runs on several data sources (Mixed) lands in the same column.
    """
    info = f"max by (host_name, deployment_environment, version) (pp_build_info{{{SEL}}})"
    rename = {"host_name": "Node", "deployment_environment": "Environment", "version": "Version"}
    overrides = []
    for key, _expr, header, field in columns:
        rename[key] = header
        if field:
            overrides.append({"matcher": {"id": "byName", "options": header}, "properties": field})
    panels.append({
        "type": "table", "title": title, "id": nid(), "datasource": DS, "description": desc,
        "gridPos": place(w, h),
        "fieldConfig": {"defaults": {"custom": {"align": "auto", "filterable": True}}, "overrides": overrides},
        "options": {"showHeader": True, "cellHeight": "sm", "sortBy": [{"displayName": "Node", "desc": False}],
                    "footer": {"show": False}},
        "transformations": [
            {"id": "joinByLabels", "options": {"value": "col",
                                               "join": ["host_name", "deployment_environment", "version"]}},
            {"id": "organize", "options": {"renameByName": rename}},
        ],
        "targets": [{"datasource": DS, "refId": key, "instant": True, "range": False, "format": "time_series",
                     "legendFormat": "",
                     "expr": f'label_replace(({expr}) * on (host_name) group_left (deployment_environment, version) '
                             f'{info}, "col", "{key}", "", "")'}
                    for key, expr, _header, _field in columns],
    })


def mapping(values):
    """Value mappings: {value: (text, color)}."""
    return [{"type": "value", "options": {str(v): {"text": t, "color": c, "index": i}
                                          for i, (v, (t, c)) in enumerate(values.items())}}]


SERVING = {"1": ("✓", "green"), "0": ("–", "text")}
REACH = {"0": ("unknown", "text"), "1": ("checking", "blue"), "2": ("reachable", "green"),
         "3": ("outbound only", "yellow"), "4": ("blocked", "red")}
REACH_CODE = " or ".join(
    f'(max by (host_name) (pp_reachability_status{{{SEL},status="{st}"}} == 1) * 0 + {n})'
    for n, st in enumerate(["unknown", "checking", "reachable", "outbound_only", "blocked"]))


H = "{{host_name}}"

row("Fleet")
stat("Nodes up", f"count(max by (host_name, deployment_environment) (pp_build_info{{{SEL}}})) or vector(0)",
     "", desc="Nodes whose /metrics reached the store in the last few minutes (pp_build_info present).")
stat("Nodes down", f"count(max by (host_name, deployment_environment) (max_over_time(pp_build_info{{{SEL}}}[1d])) "
     f"unless max by (host_name, deployment_environment) (pp_build_info{{{SEL}}})) or vector(0)", "",
     desc="Nodes seen in the last 24 h whose /metrics no longer arrive: stopped, or their collector is.", bad="red")
stat("Directory heartbeat failing",
     f"count(time() - pp_node_heartbeat_last_success_timestamp_seconds{{{SEL}}} > 600) or vector(0)", "",
     desc="Nodes with the heartbeat on (opt-in, org seeds) whose last success is over 10 min old. "
          "A node with the heartbeat off is not counted; liveness is Nodes up / down.", bad="orange")
stat("Not reachable", f'count(pp_reachability_status{{{SEL},status=~"blocked|outbound_only"}} == 1) or vector(0)', "",
     desc="Nodes whose reachability verdict is blocked or outbound only: others cannot dial them.", bad="yellow")
end_row(4)
table("Node versions", f"count by (version, deployment_environment) (pp_build_info{{{SEL}}})",
      desc="How many nodes run each version, per environment.",
      rename={"deployment_environment": "Environment", "version": "Version", "Value": "Nodes"})
ts("Machine CPU",
   [(f'1 - avg by (host_name) (rate(system_cpu_time_seconds_total{{{SEL},state="idle"}}[$__rate_interval])) '
     f"/ avg by (host_name) (system_cpu_logical_count{{{SEL}}})", H)],
   unit="percentunit", w=12, desc="Whole machine (hostmetrics): the share of CPU time not idle.")
end_row(8)

row("Nodes")
cell = {"id": "custom.cellOptions", "value": {"type": "color-text"}}
node_table("Nodes", [
    ("uptime", f"max by (host_name) (pp_process_uptime_seconds{{{SEL}}})", "Uptime",
     [{"id": "unit", "value": "dtdurations"}]),
    ("reach", REACH_CODE, "Reachability", [{"id": "mappings", "value": mapping(REACH)}, cell]),
    ("circuit_relay", f'max by (host_name) (pp_node_capability{{{SEL},service="circuit_relay"}})', "Circuit relay",
     [{"id": "mappings", "value": mapping(SERVING)}, cell, {"id": "custom.align", "value": "center"}]),
    ("media_relay", f'max by (host_name) (pp_node_capability{{{SEL},service="media_relay"}})', "Media relay",
     [{"id": "mappings", "value": mapping(SERVING)}, cell, {"id": "custom.align", "value": "center"}]),
    ("dht", f'max by (host_name) (pp_node_capability{{{SEL},service="dht"}})', "DHT",
     [{"id": "mappings", "value": mapping(SERVING)}, cell, {"id": "custom.align", "value": "center"}]),
], desc="One row per node: its environment and version (pp_build_info), uptime, reachability verdict "
        "(pp_reachability_status) and the services it is serving (pp_node_capability). Sort or filter by any "
        "column.")
end_row(10)
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
ts("Machine memory used (share)", [(f'system_memory_utilization_ratio{{{SEL},state="used"}}', H)],
   unit="percentunit", w=12, desc="Whole machine (hostmetrics): used memory as a share of installed RAM.")
ts("Container CPU (cores)",
   [(f"sum by (host_name, container_name) (rate(container_cpu_usage_nanoseconds_total{{{SEL}}}[$__rate_interval])) / 1e9",
     H + " {{container_name}}")],
   unit="suffix: cores", w=12, desc="docker_stats: CPU per container on the node, in cores.")
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


def env_variable(envs):
    if not envs:
        return {"name": "env", "label": "Environment", "type": "query", "datasource": DS,
                "query": {"query": "label_values(pp_build_info, deployment_environment)", "refId": "env"},
                "definition": "label_values(pp_build_info, deployment_environment)",
                "includeAll": True, "multi": True, "allValue": ".*", "refresh": 2, "current": {}, "hide": 0}
    return {"name": "env", "label": "Environment", "type": "custom",
            "query": ", ".join(f"{label} : {value}" for label, value in envs),
            "includeAll": True, "multi": True, "allValue": ".*",
            "current": {"text": "All", "value": "$__all"}, "options": [], "hide": 0}


def host_variable(mixed):
    if mixed:
        # A query variable reads one data source; across several, the node filter is a regex to type.
        return {"name": "host", "label": "Node (regex)", "type": "textbox", "query": ".*",
                "current": {"text": ".*", "value": ".*"}, "hide": 0}
    return {"name": "host", "label": "Node", "type": "query", "datasource": DS,
            "query": {"query": 'label_values(pp_build_info{deployment_environment=~"$env"}, host_name)', "refId": "host"},
            "definition": 'label_values(pp_build_info{deployment_environment=~"$env"}, host_name)',
            "includeAll": True, "multi": True, "allValue": ".*", "refresh": 2, "current": {}, "hide": 0}


def apply_datasources(uids):
    """No uid: the data source variable. One: fixed. Several: Mixed, each query once per data source."""
    if len(uids) == 1:
        fixed = {"type": "prometheus", "uid": uids[0]}
        for p in panels:
            if "datasource" in p:
                p["datasource"] = fixed
                for t in p["targets"]:
                    t["datasource"] = fixed
    elif len(uids) > 1:
        for p in panels:
            if "datasource" not in p:
                continue
            p["datasource"] = {"type": "datasource", "uid": "-- Mixed --"}
            targets = []
            for t in p["targets"]:
                for n, uid in enumerate(uids):
                    ref = f"{t['column']}_{n}" if "column" in t else f"Q{len(targets)}"
                    targets.append(dict(t, datasource={"type": "prometheus", "uid": uid}, refId=ref))
            p["targets"] = targets


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--datasource", action="append", default=[], metavar="UID")
    ap.add_argument("--env", action="append", default=[], metavar="LABEL=VALUE")
    ap.add_argument("--uid", default="pp-node-fleet")
    ap.add_argument("--title", default="pp-node fleet")
    ap.add_argument("--tag", action="append", default=[])
    ap.add_argument("--read-only", action="store_true")
    args = ap.parse_args()
    envs = []
    for e in args.env:
        label, sep, value = e.partition("=")
        if not sep or not label.strip() or not value.strip():
            ap.error(f"--env takes LABEL=VALUE, not {e!r}")
        envs.append((label.strip(), value.strip()))

    apply_datasources(args.datasource)
    variables = []
    if not args.datasource:
        variables.append({"name": "datasource", "label": "Data source", "type": "datasource", "query": "prometheus",
                          "current": {}, "hide": 0})
    env = env_variable(envs)
    host = host_variable(len(args.datasource) > 1)
    if len(args.datasource) == 1:
        for v in (env, host):
            if v.get("datasource") is DS:
                v["datasource"] = {"type": "prometheus", "uid": args.datasource[0]}
    variables += [env, host]

    dashboard = {
        "title": args.title,
        "uid": args.uid,
        "description": "pp-node /metrics (docs/contracts/NODE_METRICS.md) via the OpenTelemetry Collector; "
                       "host_name / deployment_environment labels come from the collector.",
        "tags": ["pp-node"] + [t for t in args.tag if t != "pp-node"],
        "timezone": "browser",
        "schemaVersion": 39,
        "version": 1,
        "editable": not args.read_only,
        "refresh": "1m",
        "time": {"from": "now-6h", "to": "now"},
        "templating": {"list": variables},
        "panels": panels,
    }
    for p in panels:
        for t in p.get("targets", []):
            t.pop("column", None)
    json.dump(dashboard, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
