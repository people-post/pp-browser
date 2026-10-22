#!/usr/bin/env python3
"""DHT lab checks, run inside the pp-dht-lab `ctl` sidecar (docker-compose.dht-lab.yml).

Phases (pp_dht_lab_smoke.sh drives them):
  seed-ma          print the seed's ADP multiaddr once it is healthy (--host: another node's)
  wait-tables      every Node is up, has published, and holds >= --min-contacts routing contacts
  find-all         every Node finds every other Node's record (lookups via GET /dht/find)

Status HTTP: http://<host>:18518 (hosts: seed, dht-1..dht-N).
"""

import argparse
import json
import sys
import time
import urllib.request

STATUS_PORT = 18518


def get_json(host, path, timeout=3.0):
    with urllib.request.urlopen(f"http://{host}:{STATUS_PORT}{path}", timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def status(host):
    try:
        return get_json(host, "/status")
    except Exception:
        return None


def adp_ma(st):
    for ma in [st.get("listen", "")] + list(st.get("listen_addrs") or []):
        if "/ip4/10.78." in ma and "/adp/" in ma:
            return ma
    return ""


def wait_until(what, check, timeout_s, every_s=1.0):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        ok, last = check()
        if ok:
            return last
        time.sleep(every_s)
    print(f"error: timed out waiting for {what}: {last}", file=sys.stderr)
    sys.exit(1)


def phase_seed_ma(args):
    def check():
        st = status(args.host)
        ma = adp_ma(st) if st else ""
        return (bool(ma), ma or st)

    print(wait_until("the seed's ADP address", check, args.timeout))


def nodes(args):
    return [f"dht-{i}" for i in range(1, args.nodes + 1)]


def dht_stats(st):
    return (st or {}).get("dht_stats") or {}


def phase_wait_tables(args):
    def check():
        short = {}
        for host in nodes(args):
            st = status(host)
            stats = dht_stats(st)
            contacts = stats.get("routing_contacts", 0)
            if not st or not stats.get("participate") or stats.get("self_publishes", 0) < 1 or contacts < args.min_contacts:
                short[host] = {"up": bool(st), "contacts": contacts, "publishes": stats.get("self_publishes", 0)}
        return (not short, short)

    wait_until(f"routing tables (>= {args.min_contacts} contacts, published)", check, args.timeout, 2.0)
    for host in nodes(args):
        stats = dht_stats(status(host))
        print(f"{host}: contacts={stats.get('routing_contacts')} records={stats.get('cached_records')} "
              f"publish_targets={stats.get('last_publish_targets')}")


def phase_find_all(args):
    peer_of = {}
    for host in nodes(args):
        st = status(host)
        if not st or not st.get("peer_id"):
            print(f"error: {host} has no peer_id", file=sys.stderr)
            sys.exit(1)
        peer_of[host] = st["peer_id"]

    def held(host):
        return {r.get("peer_id") for r in dht_stats(status(host)).get("records") or []}

    held_before = {host: held(host) for host in nodes(args)}
    wanted = {host: {peer_of[o] for o in nodes(args) if o != host} for host in nodes(args)}
    missing_before = sum(len(wanted[h] - held_before[h]) for h in nodes(args))
    print(f"records to find through lookups: {missing_before} of {sum(len(w) for w in wanted.values())}")
    if missing_before == 0:
        print("error: every record was already held — k too large for a lookup test", file=sys.stderr)
        sys.exit(1)

    asked_at = {}  # (host, peer) -> when; a lookup that missed is asked again after 30 s

    def check():
        missing = {}
        now = time.time()
        for host in nodes(args):
            gap = wanted[host] - held(host)
            for peer in gap:
                if now - asked_at.get((host, peer), 0) >= 30:
                    try:
                        get_json(host, f"/dht/find?peer_id={peer}")
                        asked_at[(host, peer)] = now
                    except Exception:
                        pass  # not answered: asked again next round
            if gap:
                missing[host] = len(gap)
        return (not missing, missing)

    wait_until("every Node to find every other Node's record", check, args.timeout, 2.0)
    print(f"all {args.nodes} Nodes hold all {args.nodes - 1} others' records "
          f"({missing_before} found by lookup, seed down)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=["seed-ma", "wait-tables", "find-all"])
    parser.add_argument("--nodes", type=int, default=12)
    parser.add_argument("--host", default="seed")
    parser.add_argument("--min-contacts", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()
    {"seed-ma": phase_seed_ma, "wait-tables": phase_wait_tables, "find-all": phase_find_all}[args.phase](args)


if __name__ == "__main__":
    main()
