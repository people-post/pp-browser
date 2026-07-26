# P2P mesh — current state

**Last updated:** 2026-07-26

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-mesh/` (n0; renamed from `libp2p-node-roles`) |
| ADRs | N001–N016 in [DECISIONS.md](DECISIONS.md) |
| Product model | Role/caps; pricing; `pp-node`; reachability; IPv6/UPnP; contact-first; listen **18517** + busy fallback (N016) |

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

## Today (before n1)

| Area | State |
|------|-------|
| Binaries | GUI only; no `pp-node` |
| Libp2p | Always listen; no bootstrap/role/caps/pricing; code default still loopback **`:40123`** until n1 → target **18517** + N016 |
| Reachability / UPnP / IPv6 prefer / friend routing | **Not implemented** |
| Busy-port fallback | **Not implemented** (listen fail → generic error, no libp2p) |
| Circuit-relay in fork | **Absent** |

## Next

1. **n1** — role shell + bootstrap + master toggle  
2. Then **np → nr → nu → n3 → nf → n4** (N015); **n2 DHT later**

## Follow-ups

See [PHASES.md](PHASES.md) and [DESIGN § Preferred delivery order](DESIGN.md#preferred-delivery-order-n015).
