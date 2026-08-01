# P2P A/V calls — current state

**Last updated:** 2026-07-31

**North star:** [NETWORKING.md](../../docs/architecture/NETWORKING.md) + **[V026](DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking)** — HTTP + libp2p only; call media on libp2p (voice-first). WebRTC/libdatachannel = **legacy in tree**.

Dogfood / codebase board for **this week**. Stable code map: [docs/architecture/CALLS.md](../../docs/architecture/CALLS.md). Product rules: [DESIGN.md](DESIGN.md) / [DECISIONS.md](DECISIONS.md).

## Landed

| Area | State |
|------|-------|
| Project docs | a3 done; **a4 thin**; **V026** libp2p-only media |
| ADRs | V001–V026 |
| a2/a3 media | Historical LAN WebRTC dogfood — **not** ongoing path |
| **a4 thin** | Soft-migrate to `media_relay` when N≥3 |
| Hop reachability | Program in [media-hop-reachability](../media-hop-reachability/) — **in-libp2p** (L1+); app `call_hop_addrs` **not** product |

## a4 thin in code (still relevant under V026)

| Area | State |
|------|-------|
| Topology | N≥3 → sticky initiator `RankMediaHops` → quote/attach → `call_sfu_attach` |
| Hop pick | Contacts ∪ org seed via `MeshHopPolicy`; PreferInCall; needs dialable **multiaddr** until L1 peerstore |
| Budgets / framing | N019 / N021 on SFU path |
| Legacy 1:1 | PeerConnection + ICE until m2 teardown |

## Still open

| Area | State |
|------|-------|
| **m1** 1:1 voice on libp2p | Next — [PHASES.md](PHASES.md) |
| Hop peerstore / circuit PeerId dial | media-hop **L1–L3** |
| App AEAD on media frames | Follow-on |
| Teardown libdatachannel | After m1 (**m2**) |
| Video on libp2p | Deferred |

## Next agent — start here

1. **m1:** Opus voice over libp2p — do not extend ICE.  
2. **media-hop L1:** peer address book in vendored libp2p / `PeerSessionManager`.  
3. Mesh [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup); confirm seed `media_relay`.  

## Agent traps

| Wrong | Right |
|-------|-------|
| Reintroduce `call_hop_addrs` / app ICE gather | H007 — reachability **in** libp2p |
| Extend libdatachannel for 1:1 | V026 — libp2p media |
| SoftMigrate invents NAT | Stack dialable? then quote |
