# Node monitoring — phases

## M1 — Registry + `/metrics` + first instruments

- [x] Registry (counter / gauge / histogram / collectors) + Prometheus text rendering
- [x] `/metrics` on the pp-node status server (bearer auth)
- [x] Process: uptime, RSS, CPU seconds, threads, open fds, build info
- [x] Runtime: owner queue depth, task wait time
- [x] Media relay: sessions, participants, frames / bytes in and out, drops by reason, quotes by result
- [x] DHT ops (circuit relay bridges moved to M2: the server exposes no count yet)
- [x] Naming contract ([NODE_METRICS.md](../../docs/contracts/NODE_METRICS.md))

## M2 — Links and reachability

- [ ] Amp links by kind, dials by outcome / error code, RTT and loss, bytes, channels by protocol
- [ ] Punches (introducer / initiator) and success, dial-back probe, rendezvous parking
- [ ] Circuit relay refusals by reason, setup latency, bytes relayed

## M3 — pp-browser UI snapshot

- [ ] Published snapshot for the GUI (call quality, network, what a desktop Node serves); Me → node panel
