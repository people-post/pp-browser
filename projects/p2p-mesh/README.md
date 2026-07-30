# P2P mesh

**Status:** **nr / nu / n3 done** — next **nf** (thin) + **n4-media** blind `media_relay` (N018); message_relay / paid pricing deferred  
**Formerly:** `projects/libp2p-node-roles/` (renamed; ADRs remain N001+)  
**Owner:** Hongwei + agents  
**Stable refs:** [docs/architecture/P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md), [docs/ops/CONFIGURATION.md](../../docs/ops/CONFIGURATION.md), [docs/architecture/PLATFORMS.md](../../docs/architecture/PLATFORMS.md), [docs/architecture/LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md)  
**Related:** [push-notifications](../push-notifications/) (HTTP Brief relay wake), [p2p-av-calls](../p2p-av-calls/) (a4 needs n4-media SFU), messaging under `src/feature/messaging/`

## One-line goal

GUI **Client/Node** mesh with capabilities and optional paid relays; **`pp-node`** for org seeds; **reachability** (IPv6/UPnP + guides); **contact-first** relay preference. Billable relays may settle on chain.

## Model (N009–N016)

| Layer | Meaning |
|-------|---------|
| **Role** | Client vs Node (`node_enabled`) |
| **Capabilities** | Checkboxes under Node (DHT, relays, chain, jobs, …) |
| **Pricing** | Design now (N010); volunteer SFU first; paid UI later (N017) |
| **Packaging** | `pp-browser` vs headless **`pp-node`** (N011) |
| **Reachability** | Status + help (N012); prefer IPv6 + UPnP (N013) |
| **Listen port** | Preferred **18517**; desktop busy fallback + persist (N016) |
| **Relay preference** | Ask/serve **contacts first**, then seed, then public (N014) |
| **Delivery order** | Reachability + circuit before DHT (N015) |

## Release scope (n1)

| In | Out |
|----|-----|
| `bootstrap_peers` + `node_enabled` | Caps / pricing / UPnP / friend routing |
| Client skips listen; bootstrap seed | DHT, circuit, media, chain, jobs |
| Me → Network master toggle | **np / nr / nu / nf** phases |
| Docs + unit tests | DNS multiaddrs; public IPFS bootstrap |

## Seed (locked)

```
/ip4/3.208.41.58/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR
```

Operated via **`pp-node`**. Desktop Node preferred listen: `/ip4/0.0.0.0/tcp/18517` (busy → fallback range + persist — N016).

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Full model, N010–N017, delivery order |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Codebase today |
| [PHASES.md](PHASES.md) | Checklists in N015 order |
| [DECISIONS.md](DECISIONS.md) | ADRs (N001+) |

## Progress snapshot (N015 order)

| Phase | Name | Status |
|-------|------|--------|
| n0 | Docs + ADRs through N015 | Done |
| n1 | Role shell, listen, bootstrap, master toggle | **Done** |
| np | `pp-node` + dial-back | **Done** |
| nr | Reachability status + manual help | **Done** |
| nu | IPv6 + UPnP/NAT-PMP | **Done** |
| n3 | Circuit-relay | **Done** |
| nf | Contact-first preference (thin; SFU rank TBD) | **Next** |
| n4-media | Blind `media_relay` (seed + desktop default on) | After nf basics — **unblocks a4** |
| n4-message / pricing | Peer message_relay; paid UI | Deferred (N017) |
| n2 | DHT | Later than circuit (N015) |
