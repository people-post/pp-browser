# P2P mesh — current state

**Last updated:** 2026-07-26

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-mesh/` (n0; renamed from `libp2p-node-roles`) |
| ADRs | N001–N016 in [DECISIONS.md](DECISIONS.md) |
| Product model | Role/caps; pricing; `pp-node`; reachability; IPv6/UPnP; contact-first; listen **18517** + busy fallback (N016) |
| **n1** | Role shell + bootstrap + Me → Network master toggle (see below) |

## n1 in code

| Area | State |
|------|-------|
| `Libp2pConfig` | `node_enabled`, `bootstrap_peers` (seed tcp/443), listen default **18517** |
| Role | `ResolveLibp2pRole` — mobile Client; desktop × `node_enabled` |
| Listen | Client skips `host->listen`; Node tries **18517–18526** then `/tcp/0`; persist bound addr |
| Errors | `MessagingHub::LastLibp2pError` surfaced in Me → Network |
| UI | Desktop **Help the network** toggle + actual listen multiaddr |
| Tests | `Libp2pRole` helpers + ConfigJson / settings merge |

## Agent traps

| Wrong | Right |
|-------|--------|
| `accept_paid_jobs` = relay monetization | Per-relay pricing (N010) |
| Org seed = GUI `--headless` | **`pp-node`** (N011) |
| “Behind firewall” as hard fact | Reachability status + soft help (N012) |
| Manual port-forward only | Prefer **IPv6 + UPnP**, then manual (N013) |
| Pick random public relay first | **Contacts first**, then seed, then public (N014) |
| Implement DHT right after n1 | Follow **N015** order (circuit/reachability before DHT) |
| Always bind 18517 or die silently | Desktop: fallback range + persist (N016); `pp-node`: fail loud |

## Still not done

| Area | State |
|------|-------|
| Binaries | GUI only; no `pp-node` (**np**) |
| Reachability / UPnP / IPv6 prefer / friend routing | **Not implemented** (nr / nu / nf) |
| Circuit-relay in fork | **Absent** (n3) |
| Caps / pricing UI | **Not implemented** (n4+) |

## Next

1. **np** — headless `pp-node` + dial-back  
2. Then **nr → nu → n3 → nf → n4**; **n2 DHT later** (N015)

## Follow-ups

See [PHASES.md](PHASES.md) and [DESIGN § Preferred delivery order](DESIGN.md#preferred-delivery-order-n015).

**Calls:** [p2p-av-calls](../p2p-av-calls/) **a0** locked parallel delivery (V010): signaling (a1) proceeds now; NAT’d media needs nr/nu/n3 + seed SFU (n4). N014 applies to media SFU hops. Org `pp-node` seeds should enable volunteer `audio_relay` / `video_relay` when those caps ship.
