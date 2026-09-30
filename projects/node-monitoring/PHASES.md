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
- [x] Amp traffic (pp-cpp-amp `Endpoint::Stats`): datagrams / bytes, rejects, reliable sent / retransmitted / lost, RTT histogram; circuit bridge bytes (`ChannelBridge::ForwardedBytes`)
- [x] Open channels by protocol: `pp_amp_channels_open` (pp-cpp-amp v2.11.0 `PeerLinkManager::CountOpenChannelsByProtocol`; fixed label set via `FixedLabelCounts`)
- [x] Rendezvous parking state: `pp_circuit_parked_relays` (`CircuitClientCoordinator::ParkedRelayCount`)

## M3 — pp-browser UI snapshot

The home is the network-status popover (not a Me panel): it already carries the helper load.

- [x] Mesh traffic in the popover: links, up / down rate, round trip, resend % (deltas between samples ≥ 1 s apart; `MeshTrafficRatesBetween`)
- [x] What a desktop Node relays for others: relaying rate (circuit bridge bytes + media relay forwarded bytes) under Helper load
- [x] Rendezvous parking in the popover: "Reachable through N relays", or none when behind NAT
- [ ] Call quality (per-call; belongs with the call screen, not the network popover)
