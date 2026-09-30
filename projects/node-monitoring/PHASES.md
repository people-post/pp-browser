# Node monitoring — phases

## M1 — Registry + `/metrics` + first instruments

- [x] Registry (counter / gauge / histogram / collectors) + Prometheus text rendering
- [x] `/metrics` on the pp-node status server (bearer auth)
- [x] Process: uptime, RSS, CPU seconds, threads, open fds, build info
- [x] Runtime: owner queue depth, task wait time
- [x] Media relay: sessions, participants, frames / bytes in and out, drops by reason, quotes by result
- [x] DHT ops (circuit relay bridges moved to M2: the server exposes no count yet)
- [x] Naming contract ([NODE_METRICS.md](../../docs/contracts/NODE_METRICS.md))

## M2 — Links, reachability, circuit relay

- [x] Amp links: active, connects by path / direction, drops by reason and stage (failed attempts), path changes
- [x] Reachability verdict and probe signals; punches started (kind, result) and served (role)
- [x] Circuit relay: requests by op / result, tunnels bridged / failed, setup latency, open tunnels, reservations
- [ ] Needs pp-cpp-amp stats: per-link RTT / loss / bytes, open channels by protocol, bytes a circuit bridge relays
- [ ] Rendezvous parking state (a pp-browser concern: `MeshConnectivity` — with M3)

## M3 — pp-browser UI snapshot

- [ ] Published snapshot for the GUI (call quality, network, what a desktop Node serves); Me → node panel
