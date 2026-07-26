# Libp2p node roles

**Status:** **n0 done** — n1 next (role shell + bootstrap + Network UI)  
**Owner:** Hongwei + agents  
**Stable refs:** [docs/architecture/P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md), [docs/ops/CONFIGURATION.md](../../docs/ops/CONFIGURATION.md), [docs/architecture/PLATFORMS.md](../../docs/architecture/PLATFORMS.md), [docs/architecture/LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md)  
**Related:** [push-notifications](../push-notifications/) (HTTP Brief relay wake), messaging under `src/feature/messaging/`

## One-line goal

**Role + capabilities + pricing policy:** **Client** (consume) vs **Node** (may host). Under Node, checkboxes enable DHT / relays / chain / optional job marketplace. **Billable relays** (message / audio / video) may be **volunteer or paid** with on-chain settle — that is the primary monetization path, not “paid jobs” alone. Desktop defaults to Node; mobile is always Client. Fixed Brief seed bootstraps the mesh.

## Model (N009 + N010)

| Layer | Control | Meaning |
|-------|---------|---------|
| **Role** | `node_enabled` | Client = no hosting; Node = listen + may run capabilities |
| **Capabilities** | Checkboxes under Node | Which infrastructure services this peer runs |
| **Pricing** | Per billable capability | Volunteer \| paid (+ rate); settle on chain — **not** a third role |
| **Paid jobs** | Optional later checkbox | Discrete task marketplace — **secondary** to relay pricing |

See [DESIGN.md § Monetization](DESIGN.md#monetization-n010--agents-read-this) before implementing economy features.

## Release scope (n1)

| In | Out |
|----|-----|
| `bootstrap_peers` + `node_enabled` | Capability / pricing flags (n2+) |
| `ResolveLibp2pRole` — mobile always Client | DHT, circuit, message/media, chain, jobs, settle |
| Client skips `host->listen` | Public IPFS bootstrap lists |
| Register seed in `PeerSessionManager` | Publishing listen addrs / AutoNAT |
| Me → Network master toggle + i18n | Half-enabled capability or pricing UI |
| Docs + unit tests | DNS multiaddrs |

## Seed (locked)

```
/ip4/3.208.41.58/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR
```

TCP **443** = libp2p transport on the seed (≠ HTTPS Brief API). Desktop Node listen: `/ip4/0.0.0.0/tcp/40123`.

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Role, capabilities, **monetization (N010)**, bootstrap, n1 |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Codebase today (before n1) |
| [PHASES.md](PHASES.md) | n0–n4+ checklist |
| [DECISIONS.md](DECISIONS.md) | ADRs (N001+) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| n0 | Project docs + ADRs (N009, N010) | Done |
| n1 | Role shell, listen, bootstrap, master toggle | Next |
| n2 | DHT + checkbox | Deferred |
| n3 | Circuit-relay + checkbox | Deferred |
| n4+ | Relays + **pricing**; chain; later paid-jobs marketplace | Vision |
