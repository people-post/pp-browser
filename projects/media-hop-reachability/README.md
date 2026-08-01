# Media hop reachability

**Status:** **Docs / program** — implement **inside vendored libp2p** (not app-layer NAT); consume from SoftMigrate later  
**Owner:** Hongwei + agents  
**Related:** [p2p-mesh](../p2p-mesh/) (N022), [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md), [NETWORKING.md](../../docs/architecture/NETWORKING.md), [p2p-av-calls](../p2p-av-calls/) (V026), [CALLS.md](../../docs/architecture/CALLS.md)

## One-line goal

Peers (and SoftMigrate) can **dial a media hop by PeerId** because the **libp2p stack** knows how to find and reach it — address lifecycle, NAT, circuit — not because call signaling invents a second dial toolkit.

## Ownership (locked)

| Layer | Owns |
|-------|------|
| **Vendored libp2p + `libp2p/integration`** | Discovery, listen/observed addrs, reachability probes, circuit, dial |
| **p2p-mesh policy** | Who may be a hop (contacts ∪ seed), budgets, incentives, settle |
| **p2p-av-calls** | SoftMigrate / attach **consume** “is dialable?” |
| **This project** | Spec + phases for the **in-stack** program; thin consume notes — **not** a forever app protocol |

**Out of scope here:** App-only `call_hop_addrs` / ICE-like gather over chat (removed from product direction; do not reintroduce).

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | In-stack model, consume API, phases |
| [DECISIONS.md](DECISIONS.md) | ADRs (H001+) |
| [PHASES.md](PHASES.md) | L0–L5 checklists (libp2p-first) |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Gap vs fork today |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| L0 | Docs + ownership (this project) | **In progress** |
| L1 | Address book / Identify / persist known mas in host | Not started |
| L2 | Reachability + UPnP wired into advertised addrs | Partial in mesh; not unified |
| L3 | Circuit → PeerId-friendly dial (evolve custom bridge) | Custom n3 exists; not v2 |
| L4 | SoftMigrate consumes stack dialability only | Not started |
| L5 | Directory / DHT when N015 allows | Later |
