# Stress lab: one pp-node, many clients

**Tier:** ops

How much one node serves, and what clients do when it is full. Levels and their meanings: [p2p-mesh N033](../../projects/p2p-mesh/DECISIONS.md#n033--nodes-say-how-busy-they-are-per-service-clients-act-on-the-level); the node's metrics: [NODE_METRICS.md](../contracts/NODE_METRICS.md).

## Running it

```bash
cmake -B build-pp-node -S . -DCMAKE_BUILD_TYPE=Release && ninja -C build-pp-node pp-node pp-node-load
scripts/test/pp_node_stress.py park --build build-pp-node --clients 8000 --ramp 150 --duration 120 --procs 8 --renew 10
scripts/test/pp_node_stress.py calls --build build-pp-node --calls 50 --members 4 --duration 45 --procs 8
scripts/test/pp_node_stress.py bridges --build build-pp-node --pairs 100 --duration 45 --procs 4
```

- **The node** is `--build`'s pp-node (a Release build: `build-pp-node`; the script refuses a Debug one), pinned to `--node-cores` (default `0-1`: a two-core seed) with `taskset`, on loopback. It is isolated: no seeds (an unroutable TEST-NET bootstrap, as the docker labs use), no directory, no publishing. The run stops if it ever links to anything but this host.
- **The load** is `pp-node-load` (`src/app/node/tools/load/`), on `--load-cores`. Each simulated client is its own Amp peer (identity, UDP socket):
  - `park`: associate, reserve, renew every `--renew` s (10: a call's cadence);
  - `calls`: `--calls` group calls of `--members` on the node's media_relay, each member sending audio (40 ms frames) and video (`--video-kbps` 500, `--fps` 15, ~1100 B packets) and subscribing to the others;
  - `bridges`: relayed 1:1 pairs — a parked target, a dialer bridged to it by PeerId, the same media both ways. The pairs' channel is the probe's reliable one, not a call's best-effort carrier: its numbers are a worst case for real relayed calls.
- **Out:** a line a second (node CPU, RSS, links, its three levels; the clients' parked / attached / bridged, frames, refusals by level, latency p95), a summary, and `--out` JSON with the node's watched metrics each second.
- **Buffers:** the lab host's `net.core.rmem_max` caps what the node's socket gets ([BUILD.md § pp-node](BUILD.md#headless-mesh-node-pp-node)); `pp_udp_receive_buffer_bytes` shows it.

## Measured (Release, two cores, 208 KiB socket buffers; parked 2026-11-08 with pp-cpp-amp v5.1.0, the rest 2026-11-07)

| Load | Holds up to | Then |
|------|-------------|------|
| Parked clients, renewing every 10 s | 20000: 46 % of a core (6000: 5 %, 12000: 17 %; ~20 KB a link) | Past ~23k the pump saturates (level full) and the node sheds load — badly (5). Default capacity 16384 links ([CONFIGURATION.md](CONFIGURATION.md#node-capacity-meshcapacity)). Under Amp v5.0.3, whose walks over every link ran every pump round, 6000 cost ~40 % of a core and the ceiling was ~11k |
| Group calls, 4 members, 500 kbps video + 32 kbps audio | 50 calls (200 members): ~100 % delivered, 23 % CPU, pump 34 % busy, level open | 100 calls: kernel drops begin (the socket buffer, not the CPU: pump 40 % busy), 98 % delivered, level busy → full. 150: 88 %, new attaches refused `full` |
| Relayed pairs (reliable channel) | 100 pairs (105 Mbps): 100 %, 25 % CPU, level open | 200 pairs: kernel drops, 92 %, level full. 300: 28 % (the probe's channel resends into the drops) |

The node says `busy` / `full` before its members feel it, and new work goes elsewhere ([TEST_STRATEGY § A full seed](TEST_STRATEGY.md)). On these loads a Release node is limited by its socket buffers, not its CPU: run it on a host with `99-pp-node-udp.conf` before calling a number a seed's.

**Earlier figures** (2026-11-06, in this file until 2026-11-07) were measured on a Debug build, several times slower: ~2000 parked, 50 calls 79 % → 100 % after the pump fix, 100 pairs 23 % → 100 %. The pump fix (pp-cpp-mesh v6.2.0) stands; the capacities do not. `pp_node_stress.py` now refuses a non-Release build.

## What limits a node

1. **Socket buffers** — on Release the first limit for media: the OS default (208 KiB) overflows in bursts while the pump has room. Kernel drops are `pp_udp_receive_drops` and a pressure. Hosts should raise the cap (`packaging/pp-node/99-pp-node-udp.conf`).
2. **One pump thread does the data plane**, sealing every fan-out copy per recipient (XChaCha20-Poly1305), and walking the link table on its 50 ms cadence: past ~23k parked links it saturates one core while the worker pool idles. Its busy share is `pp_io_busy_fraction` and a pressure on every level.
3. **Media waits without an age limit.** Call media is best effort on every normal path ([V063](../../projects/p2p-av-calls/DECISIONS.md#v063--call-audio-is-best-effort-the-codec-recovers-losses)), but a frame can wait in a link's send queue (the mux holds up to 32 MiB) for seconds; under overload media arrives late instead of being dropped.
4. **New links cost a handshake** (ML-DSA): ~300–400 new clients a second on two cores. While the pump is saturated, a node refuses new inbound links before their handshake (`pp_link_inbound_refused_busy`), so the CPU stays with the links it has.
5. **An overloaded node can lose parked clients**: once the pump saturates, renewals and keepalives run late, links time out, and their clients redial into the same overload (32k offered with capacities raised to 40k: ~23k parked, then ~2k). The defaults (8192 parks) keep a node below that; clients that ignore "full" are what the handshake gate (4) is for.

## Restarts and storms (2026-11-10, Release, two cores, phones renewing every 20 s as the app does)

`pp_node_stress.py --restart-at T [--restart-kill]` restarts the node on the same data (the same identity) mid-run; `pp-node-load` park clients behave as the app's park does — a 30 s hold, a lost link dialed again next round, a relay that said full skipped while that is fresh (`--ignore-full` for an old or abusive client).

| Case | 8000 phones offered (7783 parked: the 8192 reservations' full line) |
|------|------|
| Clean restart (SIGTERM) | Reservations back to 50 % in 12 s, 90 % in 20 s, 99 % in 22 s; peak 59 % CPU. The node's Close reaches every phone; each parks again at its next round |
| Crash (SIGKILL) | 50 % in 42 s, 99 % in 50 s; peak 43 % CPU. Nothing tells the phones: a phone notices only when its link's liveness window ends (25 s), then parks at its next round. Phones park on two relays in prod, so one seed's crash leaves them reachable through the other |
| 20000 offered, app clients | Calm: 13–35 % CPU, no drops, every parked phone kept. Refused phones skip a full node for 2 min |
| 20000 offered, `--ignore-full` | Without the handshake gate: 100 % CPU, ~2.3k kernel drops a second, the parked phones at the edge. With it: the gate closes at saturation, then 15–20 % CPU, no drops, every parked phone kept |

Found on the way: a phone whose seed was full, then crashed, skipped the restarted — empty — seed for the 2 min the old "full" stayed fresh (130 s to recover instead of 50). The circuit client now forgets a relay's park level when the last park it held there ends without its cancel (pp-cpp-mesh v6.6.0).

## Open

- A deadline on real-time media in the send queue (3), and the data plane on more than one thread (2): a design of their own.
- What is left of the walks (2): perf at 15k parked puts ~45 % of the node's CPU in the 50 ms walk (each mux's tick and its keepalive tier) and ~11 % in per-event scans (a link's drop, an inbound's adoption). A cached keepalive tier and indexed scans would cut most of it.
- Thresholds per machine type (the 0.70 / 0.95 lines are first guesses); a run on the org seeds' size with the host's buffers raised.
- Crash recovery is detection: a stateless reset (a restarted node tells a phone its association is gone, with a token from the handshake so it cannot be forged) would bring a crash to a clean restart's ~20 s. A wire change of its own.
- A soak, and abuse (one client's uncapped bandwidth through a bridge).
