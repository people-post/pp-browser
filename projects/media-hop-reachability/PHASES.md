# Media hop reachability — phases

Implement **in libp2p** ([H001](DECISIONS.md#h001--separate-project-implementation-in-libp2p)). SoftMigrate consume comes last.

## L0 — Docs + ownership

- [x] README / DESIGN / DECISIONS / PHASES / CURRENT_STATE
- [x] H007 — no app `call_hop_addrs` product path; remove uncommitted prototype
- [x] Cross-link NETWORKING / N022 / V026 / CALLS

## L1 — Peer address book in stack

- [ ] Host/peerstore: remember multiaddrs per PeerId (Identify, successful dial, bootstrap)
- [ ] TTL / replace stale; expose to `PeerSessionManager` / dial helpers
- [ ] Document in [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md)

## L2 — Advertise Reachable listen set

- [ ] Unify listen + UPnP external + DialBack-observed into advertised Identify addrs
- [ ] Node + `media_relay` on ⇒ hop candidates get fresh ads when peers connect

## L3 — Circuit PeerId-friendly dial

- [ ] Evolve custom circuit toward dial-by-PeerId when relay already has target (or reservation)
- [ ] SoftMigrate may use circuit when direct `IsDialable` fails (H005)

## L4 — SoftMigrate consume stack only

- [ ] Rank hops as today; skip if stack says undialable
- [ ] Drop reliance on empty contact ma as the only signal (contacts optional cache — H003)
- [ ] Dogfood: Android SoftMigrate → Windows Node without pasted multiaddr

## L5 — Directory / DHT (later)

- [ ] Per N015 timing; still closed-set for media hops (N020)
