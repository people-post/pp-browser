# Stress lab: one pp-node, many clients

**Tier:** ops

How much one node serves, and what clients do when it is full. Levels and their meanings: [p2p-mesh N033](../../projects/p2p-mesh/DECISIONS.md#n033--nodes-say-how-busy-they-are-per-service-clients-act-on-the-level); the node's metrics: [NODE_METRICS.md](../contracts/NODE_METRICS.md).

## Running it

```bash
ninja -C build pp-node pp-node-load
scripts/test/pp_node_stress.py park --clients 2000 --ramp 100 --duration 75 --procs 4 --renew 10
scripts/test/pp_node_stress.py calls --calls 50 --members 4 --duration 45 --procs 8
scripts/test/pp_node_stress.py bridges --pairs 100 --duration 45 --procs 4
```

- **The node** is `build/`'s pp-node, pinned to `--node-cores` (default `0-1`: a two-core seed) with `taskset`, on loopback. It is isolated: no seeds (an unroutable TEST-NET bootstrap, as the docker labs use), no directory, no publishing. The run stops if it ever links to anything but this host.
- **The load** is `pp-node-load` (`src/app/node/tools/load/`), on `--load-cores`. Each simulated client is its own Amp peer (identity, UDP socket):
  - `park`: associate, reserve, renew every `--renew` s (10: a call's cadence);
  - `calls`: `--calls` group calls of `--members` on the node's media_relay, each member sending audio (40 ms frames) and video (`--video-kbps` 500, `--fps` 15, ~1100 B packets) and subscribing to the others;
  - `bridges`: relayed 1:1 pairs — a parked target, a dialer bridged to it by PeerId, the same media both ways. The pairs' channel is the probe's reliable one, not a call's best-effort carrier: its numbers are a worst case for real relayed calls.
- **Out:** a line a second (node CPU, RSS, links, its three levels; the clients' parked / attached / bridged, frames, refusals by level, latency p95), a summary, and `--out` JSON with the node's watched metrics each second.
- **Buffers:** the lab host's `net.core.rmem_max` caps what the node's socket gets ([BUILD.md § pp-node](BUILD.md#headless-mesh-node-pp-node)); `pp_udp_receive_buffer_bytes` shows it.

## Measured (2026-11-06, two cores, 208 KiB socket buffers)

| Load | Holds up to | Then |
|------|-------------|------|
| Parked clients, renewing every 10 s | ~1990 | The link table (2048) is the limit, not CPU (~0.35 core). Busy from ~1500 links, full from ~1950: new parks refused `busy` + `full`; no parked client evicted (a held reservation keeps its link) |
| Group calls, 4 members, 500 kbps video + 32 kbps audio | 50 calls (200 members, 316 Mbps out): 100 % delivered, p95 0.75 s, level busy | 100 calls: the pump saturates (busy 0.98), level full, new attaches refused — but the members already in degrade (60 %, p95 27 s) |
| Relayed pairs (reliable channel) | 100 pairs (105 Mbps): 100 %, p95 9 ms, level busy | 200 pairs: kernel drops (~11 k/s), level full, 16 % delivered |

Before the pump fix (pp-cpp-mesh v6.2.0) the node collapsed at half these loads and said `open` throughout: 50 calls 79 % delivered with p95 22.6 s; 100 pairs 23 % with 32 s.

## What limits a node

1. **One pump thread does the data plane**, sealing every fan-out copy per recipient (XChaCha20-Poly1305): group calls saturate one core while the worker pool idles. Its busy share is `pp_io_busy_fraction` and a pressure on every level.
2. **Socket buffers** — the OS default (208 KiB) overflows in bursts; kernel drops are `pp_udp_receive_drops` and a pressure. Hosts should raise the cap (`packaging/pp-node/99-pp-node-udp.conf`).
3. **Media waits without an age limit.** Call media is best effort on every normal path ([V063](../../projects/p2p-av-calls/DECISIONS.md#v063--call-audio-is-best-effort-the-codec-recovers-losses)), but a frame can wait in a link's send queue (the mux holds up to 32 MiB) for seconds; under overload media arrives late instead of being dropped.
4. **The link table** (2048) caps clients per node.

## Open

- A deadline on real-time media in the send queue (3), and the data plane on more than one thread (1): a design of their own.
- Thresholds per machine type (the 0.70 / 0.95 lines are first guesses); a run on the org seeds' size with the host's buffers raised.
- Restart storms (every parked client redialing at once), a soak, and abuse (one client's uncapped bandwidth through a bridge).
