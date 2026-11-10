#!/usr/bin/env python3
"""Stress lab: one pp-node, many clients (docs/ops/STRESS_LAB.md).

Starts an isolated pp-node from build/ (no bootstrap seeds, no directory, no publishing — it never talks to anything
but its load), pinned to --node-cores so it stands in for a seed of that size; runs pp-node-load processes on the
other cores; reads the node's /metrics once a second; prints a timeline and a summary, and writes both as JSON.

  scripts/test/pp_node_stress.py park --clients 2000 --ramp 100 --duration 120
  scripts/test/pp_node_stress.py calls --calls 30 --members 4 --duration 90
  scripts/test/pp_node_stress.py bridges --pairs 100 --duration 90

--capacity sizes the node (mesh.capacity: links, reservations, bridges, group_calls). Extra pp-node-load flags pass through (--video-kbps, --fps, --renew, …). --procs splits the clients over several
load processes (each with its own threads), for runs bigger than one process pumps comfortably.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# An unroutable test address (TEST-NET-3), as the docker labs use: replaces the built-in seeds, dials nowhere.
LAB_BOOTSTRAP = "/ip4/203.0.113.1/udp/443/adp/1.0.0/p2p/QmW6e19wEv2FozTzQXxCee7USqQLE7HAvcF77es4brdSWd"
NODE_CONFIG = {
    "mesh": {
        "mesh_publish": False,
        "mesh_heartbeat": False,
        "capabilities": {"circuit_relay": True, "media_relay": True, "dht": False},
    },
    "registration": {"base_url": ""},
}
WATCHED = [
    "pp_service_load", "pp_service_pressure", "pp_link_active", "pp_circuit_relay_reservations",
    "pp_circuit_relay_tunnels", "pp_media_relay_sessions", "pp_media_relay_participants",
    "pp_media_relay_dropped_frames_total", "pp_media_relay_frames_total", "pp_process_cpu_seconds",
    "pp_process_resident_memory_bytes", "pp_runtime_queue_depth", "pp_amp_rejected_datagrams_total",
    "pp_link_drops_total", "pp_io_busy_fraction", "pp_udp_receive", "pp_link_connects_total", "pp_amp_datagrams_total",
    "pp_amp_reliable_packets_total", "pp_circuit_relay_requests_total", "pp_amp_channels_open",
    "pp_link_inbound_refused_busy",
]
METRIC_LINE = re.compile(r'^([a-z_]+)(\{[^}]*\})?\s+([-0-9.eE+]+)$')


def read_metrics(url):
    """{name{labels}: value} for the watched metrics."""
    out = {}
    try:
        with urllib.request.urlopen(url, timeout=2) as resp:
            text = resp.read().decode()
    except OSError:
        return out
    for line in text.splitlines():
        m = METRIC_LINE.match(line)
        if m and any(m.group(1).startswith(w) for w in WATCHED):
            out[m.group(1) + (m.group(2) or "")] = float(m.group(3))
    return out


def build_type(build):
    """CMAKE_BUILD_TYPE of a build dir ("" when unknown)."""
    try:
        with open(os.path.join(build, "CMakeCache.txt")) as f:
            for line in f:
                if line.startswith("CMAKE_BUILD_TYPE:"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


def start_node(args, work):
    data = os.path.join(work, "node")
    os.makedirs(data, exist_ok=True)  # a restart keeps the node's data: the same identity, as a seed's restart
    config = os.path.join(work, "node.json")
    node_config = json.loads(json.dumps(NODE_CONFIG))
    if args.capacity:
        node_config["mesh"]["capacity"] = {k: int(v) for k, v in (kv.split("=", 1) for kv in args.capacity.split(","))}
    with open(config, "w") as f:
        json.dump(node_config, f)
    env = dict(os.environ, PP_BROWSER_PIN="stress-lab", PP_NODE_DATA_DIR=data,
               PP_NODE_AMP_UDP_PORT=str(args.port), PP_NODE_STATUS_ADDR="127.0.0.1:%d" % (args.port + 1),
               PP_NODE_BOOTSTRAP_PEERS=LAB_BOOTSTRAP)
    log = open(os.path.join(work, "node.log"), "a")
    cmd = ["taskset", "-c", args.node_cores, os.path.join(args.build, "src/app/node/pp-node"), "--config", config]
    proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    status = "http://127.0.0.1:%d" % (args.port + 1)
    for _ in range(60):
        try:
            with urllib.request.urlopen(status + "/status", timeout=1) as resp:
                peer = json.load(resp)["peer_id"]
                return proc, "/ip4/127.0.0.1/udp/%d/adp/2/p2p/%s" % (args.port, peer), status
        except (OSError, KeyError, ValueError):
            time.sleep(0.5)
    proc.kill()
    sys.exit("pp-node did not come up (log: %s)" % log.name)


def restart_node(args, work, node):
    """Stop (or kill) the node and start it again on the same data dir and port."""
    if args.restart_kill:
        node.kill()
    else:
        node.terminate()
    try:
        node.wait(timeout=20)
    except subprocess.TimeoutExpired:
        node.kill()
        node.wait()
    time.sleep(args.restart_down)
    proc, _, _ = start_node(args, work)
    return proc


def local_addresses():
    """This host's own addresses (a load client can reach the node through any of them)."""
    out = {"127.0.0.1", "::1"}
    try:
        text = subprocess.run(["ip", "-o", "addr"], capture_output=True, text=True, check=False).stdout
    except OSError:
        return out
    for line in text.splitlines():
        parts = line.split()
        if len(parts) > 3:
            out.add(parts[3].split("/")[0])
    return out


LOCAL = local_addresses()


def assert_isolated(work):
    """The node linked to nothing but this host (its load): a lab node must never reach a real seed."""
    with open(os.path.join(work, "node.log"), errors="replace") as f:
        for line in f:
            m = re.search(r"link connected .* remote=([^ ]+)", line)
            if not m:
                continue
            host = m.group(1).rsplit(":", 1)[0].strip("[]")
            if host not in LOCAL and not host.startswith("127."):
                sys.exit("stress node linked to %s — not isolated, stopping" % m.group(1))


def run_load(args, hop, extra):
    """The pp-node-load processes; returns their Popen objects."""
    procs = []
    per = {"park": "clients", "calls": "calls", "bridges": "pairs"}[args.mode]
    total = getattr(args, per)
    share = max(1, total // args.procs)
    for i in range(args.procs):
        n = share if i < args.procs - 1 else total - share * (args.procs - 1)
        cmd = ["taskset", "-c", args.load_cores, os.path.join(args.build, "src/app/node/pp-node-load"), "--hop", hop,
               "--mode", args.mode, "--" + per, str(n), "--ramp", str(args.ramp / args.procs), "--duration",
               str(args.duration), "--threads", str(args.threads), "--tag", "p%d" % i] + extra
        if args.mode == "calls":
            cmd += ["--members", str(args.members)]
        procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True))
    return procs


def collect(procs):
    """Per process: its JSON lines by second (read on a thread each)."""
    lines = [dict() for _ in procs]

    def reader(i, p):
        for raw in p.stdout:
            try:
                rec = json.loads(raw)
            except ValueError:
                continue
            lines[i]["summary" if rec.get("summary") else rec["t"]] = rec

    threads = [threading.Thread(target=reader, args=(i, p), daemon=True) for i, p in enumerate(procs)]
    for t in threads:
        t.start()
    return lines, threads


SUMMED = ["started", "linked", "link_fail", "relinked", "skipped_full", "parked", "reserve_ok", "reserve_fail", "attached", "attach_fail",
          "bridged", "bridge_fail", "tx_frames", "tx_bytes", "rx_frames", "rx_bytes", "lat_n"]


def merge(records):
    out = {k: sum(r.get(k, 0) for r in records) for k in SUMMED}
    out["lat_p95_ms"] = max((r.get("lat_p95_ms", 0) for r in records), default=0)
    out["lat_p50_ms"] = max((r.get("lat_p50_ms", 0) for r in records), default=0)
    refused = {}
    for r in records:
        for k, v in r.get("refused", {}).items():
            refused[k] = refused.get(k, 0) + v
    out["refused"] = refused
    return out


def level_word(v):
    return {1: "open", 2: "busy", 3: "full", 4: "draining"}.get(int(v), "-")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["park", "calls", "bridges"])
    ap.add_argument("--clients", type=int, default=500)
    ap.add_argument("--calls", type=int, default=10)
    ap.add_argument("--members", type=int, default=4)
    ap.add_argument("--pairs", type=int, default=20)
    ap.add_argument("--ramp", type=float, default=50, help="clients (or calls' members, or pairs) started a second")
    ap.add_argument("--duration", type=int, default=120)
    ap.add_argument("--procs", type=int, default=1)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--node-cores", default="0-1", help="taskset cores for the node (its size)")
    ap.add_argument("--load-cores", default="4-23")
    ap.add_argument("--port", type=int, default=19517)
    ap.add_argument("--capacity", help="the node's mesh.capacity, e.g. links=20000,reservations=20000")
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--out", help="write the timeline + summary JSON here")
    ap.add_argument("--keep", action="store_true", help="keep the work dir (node log)")
    ap.add_argument("--restart-at", type=int, default=0,
                    help="restart the node this many seconds into the run (same data: the same identity)")
    ap.add_argument("--restart-kill", action="store_true", help="the restart is a crash (SIGKILL), not a stop")
    ap.add_argument("--restart-down", type=float, default=2.0, help="seconds the node stays down")
    ap.add_argument("--allow-debug", action="store_true", help="run a non-Release build (its numbers are not a seed's)")
    args, extra = ap.parse_known_args()
    kind = build_type(args.build)
    if kind not in ("Release", "RelWithDebInfo") and not args.allow_debug:
        # A Debug node is several times slower: its numbers say nothing about a seed (2026-11-07).
        sys.exit("%s is a %s build: measure a Release one (e.g. build-pp-node), or pass --allow-debug" %
                 (args.build, kind or "unknown"))

    work = tempfile.mkdtemp(prefix="pp-stress-")
    node, hop, status = start_node(args, work)
    timeline = []
    try:
        procs = run_load(args, hop, extra)
        lines, readers = collect(procs)
        t0 = time.time()
        prev_cpu = None
        restarted = False
        while any(p.poll() is None for p in procs):
            time.sleep(1)
            t = int(time.time() - t0)
            if args.restart_at and not restarted and t >= args.restart_at:
                restarted = True
                node = restart_node(args, work, node)
                prev_cpu = None
                print("t=%3ds node restarted (%s, down %.1fs)" % (t, "killed" if args.restart_kill else "stopped",
                                                                   args.restart_down), flush=True)
            m = read_metrics(status + "/metrics")
            cpu = m.get("pp_process_cpu_seconds", 0)
            cpu_pct = (cpu - prev_cpu) * 100 if prev_cpu is not None else 0
            prev_cpu = cpu
            clients = merge([l[max(k for k in l if isinstance(k, int))]
                             for l in list(lines) if any(isinstance(k, int) for k in l)])
            row = {"t": t, "node_cpu_pct": round(max(cpu_pct, 0.0), 1), "restarted": restarted,
                   "node_rss_mb": round(m.get("pp_process_resident_memory_bytes", 0) / 1e6, 1),
                   "links": m.get("pp_link_active", 0), "levels": {}, "pressure": {}, "clients": clients,
                   "node": m}
            for svc in ("park", "relay", "media_relay"):
                row["levels"][svc] = level_word(m.get('pp_service_load{service="%s"}' % svc, 0))
                row["pressure"][svc] = round(m.get('pp_service_pressure{service="%s"}' % svc, 0), 3)
            timeline.append(row)
            print("t=%3ds cpu=%5.1f%% rss=%6.1fMB links=%5d park=%-8s relay=%-8s media=%-8s | parked=%d attached=%d "
                  "bridged=%d rx=%d refused=%s lat_p95=%.0fms" % (
                      t, row["node_cpu_pct"], row["node_rss_mb"], row["links"], row["levels"]["park"],
                      row["levels"]["relay"], row["levels"]["media_relay"], clients["parked"], clients["attached"],
                      clients["bridged"], clients["rx_frames"], clients["refused"], clients["lat_p95_ms"]),
                  flush=True)
            assert_isolated(work)
        for r in readers:
            r.join(timeout=5)
        summary = merge([l["summary"] for l in lines if "summary" in l])
        print("summary:", json.dumps(summary))
        if args.out:
            with open(args.out, "w") as f:
                json.dump({"args": vars(args), "extra": extra, "timeline": timeline, "summary": summary}, f, indent=1)
    finally:
        node.terminate()
        try:
            node.wait(timeout=10)
        except subprocess.TimeoutExpired:
            node.kill()
        if args.keep:
            print("work dir:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
