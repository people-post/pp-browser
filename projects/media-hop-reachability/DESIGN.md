# Media hop reachability — design

**Authoritative specification** for dial-by-PeerId inside the **Amp mesh** stack.  
**Execution order:** [PHASES.md](PHASES.md). **What's landed:** [CURRENT_STATE.md](CURRENT_STATE.md). **Rationale:** [DECISIONS.md](DECISIONS.md).

**Related:** [p2p-mesh](../p2p-mesh/DESIGN.md) (relay **policy**), [RELAY_SCOPE.md](../p2p-mesh/RELAY_SCOPE.md) (scope), [NETWORKING.md](../../docs/architecture/NETWORKING.md), [CALLS.md](../../docs/architecture/CALLS.md), [MESH.md](../../docs/architecture/MESH.md)

---

## Overview

Peers (and SoftMigrate) must **dial a media hop by PeerId** under real NATs. That dialability story lives in **Amp + `src/domain/mesh/`** (`MeshHost`, reachability, circuit) — not in call signaling or a second NAT toolkit.

SoftMigrate **selects** hops and opens `media_relay`; reachability is a **stack precondition**. Hop relays stay **blind**; app E2E call keys remain in the call layer.

```mermaid
flowchart TB
  subgraph stack [Amp mesh + domain/mesh]
    Ch0[ch0 / addr ads]
    Book[Peer address book]
    Reach[Reachability / DialBack / UPnP]
    Punch[Amp Coordinated Punch]
    Circuit[Circuit bridge]
    Dial[Connect by PeerId]
  end
  subgraph product [App / mesh policy]
    Rank[RankMediaHops / scope escalation]
    Media[media_relay quote attach]
  end
  Ch0 --> Book
  Reach --> Book
  Punch --> Book
  Book --> Dial
  Punch --> Dial
  Circuit --> Dial
  Rank -->|"peer_id if dialable"| Dial
  Dial --> Media
```

---

## Goals

1. **Dial a media hop by PeerId** using Amp mesh reachability (publish, punch, circuit).
2. Preserve **blind** hop semantics + app E2E call keys (see [p2p-av-calls](../p2p-av-calls/DECISIONS.md)).
3. One peer stack — app layers **consume** dialability; they do not reimplement NAT traversal.

## Non-goals

- App protocols that mimic ICE (`call_hop_addrs`, STUN-over-signaling).
- WebRTC for hop dial.
- Open public hop market (mesh policy — [p2p-mesh](../p2p-mesh/DESIGN.md)).
- Relay scope, bridge score, pricing, or settle (mesh — [RELAY_SCOPE.md](../p2p-mesh/RELAY_SCOPE.md)).
- Putting SoftMigrate / pricing inside Amp or the punch coordinator.
- Reviving libp2p Host/TCP DCUtR as the product punch path (A017).

---

## Ownership

| Layer | Owns |
|-------|------|
| **This project (stack)** | Discovery, listen/observed addrs, reachability probes, peer address book, **Amp Coordinated Punch**, circuit evolution, `IsPeerDialable` |
| **[p2p-mesh](../p2p-mesh/)** | Who may be a hop (contacts ∪ seed), relay scope, budgets, incentives, settle; who may act as punch introducer (policy) |
| **[p2p-av-calls](../p2p-av-calls/)** | SoftMigrate / attach **consume** “is dialable?” |
| **HTTP Brief** | Message inbox durability (not hop dial) |

Do **not** expand this project to cover scope routing or incentives — that splits ownership locked in H001.

---

## Problem

| Stack should own | App/mesh still owns |
|------------------|---------------------|
| Listen + observed multiaddrs | Hop **eligibility** (contacts ∪ seed) |
| Persist / refresh peer addr book | Quote / budgets / settle |
| Circuit / **Amp Coordinated Punch** | Who picks SoftMigrate initiator |
| `Connect(peer_id)` success/fail | `media_relay` framing |

SoftMigrate often fails with **PeerId known, multiaddr empty** because addrs live only in contacts/bootstrap — the host must own a full dialability story for arbitrary hop PeerIds.

---

## Stack capabilities

### Address book

Store multiaddrs per PeerId from ch0 ads, dial success, bootstrap, dial-back-observed, punch success; TTL / replace stale entries. Expose to dial helpers / hop reach. Contacts may **mirror** stack addrs as a TTL UX cache — not the source of truth (H003).

### Advertise

Unify listen + UPnP external + DialBack-observed addrs into Amp ch0 / advertised sets. Nodes with `media_relay` on should publish fresh ads when peers connect.

### Amp Coordinated Punch

DCUtR-**shaped**, Amp-native coordinated simultaneous dial via an introducer that already has Sessions to both peers. Spec: [HOLE_PUNCH.md](HOLE_PUNCH.md). ADR: [H009](DECISIONS.md#h009--amp-coordinated-punch-acp).

Keepalive / `MaybeLearnPath` maintain established paths; punch **creates** (or promotes) direct PeerLinks.

### Circuit bridge

Evolve custom circuit toward **PeerId-first** dial when a relay already has the target (or reservation).

| Mode | Description | Maturity |
|------|-------------|----------|
| **Single-hop** | One relay direct-dials target | Shipped |
| **Multi-hop v2** | Transitive paths via `bridge_path`, nested subcontract | Planned — [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md) |

**Preference order:** stack address book + Reachable ads → **Amp Coordinated Punch** → circuit → SoftMigrate failure (H002). Circuit may enable dial to hop PeerId; prefer contact then seed bridges.

**Billing note:** Direct attach (A pays hop B) vs brokered attach (A pays immediate relay R1 only when R1 orchestrates path) — see H005 and mesh N024. Successful punch favors **direct attach**.

Punch and multi-hop are **parallel** tracks: punch for punchable NATs; multi-hop for unpunchable partitions.

---

## Consume API

Policy stays in mesh (`MeshHopPolicy`, scope escalation); dialability comes from the stack:

```text
// Tentative — refine next to MeshHost / hop reach helpers:
bool IsPeerDialable(PeerId);
optional<Multiaddr> PreferredDialAddr(PeerId);  // may be empty if circuit-only path

// SoftMigrate loop:
for hop in RankMediaHops(...):
  if !IsPeerDialable(hop) continue;
  RegisterEndpoint / Connect + RequestQuote...
```

Exact C++ types live next to `MeshHost` / `AmpCircuitHopReach`.

---

## Publish vs gather

**Durable path:** Nodes that are Reachable (or successfully punched) keep the stack’s address book fresh (and optionally mirror into contacts for UX).

**Rejected path:** Mid-call multiaddr request/reply over `call_*` as the primary design (H007). No WebRTC / app STUN for hops (H004).

---

## Privacy

Stack-held addrs are shared with peers that dial, punch via introducer, or exchange ch0 ads — same class as today’s contact multiaddrs. Admission for circuit/media/introducer remains mesh policy (contact-first, scope caps).

---

## Failure UX

SoftMigrate: skip undialable hops; aggregated error if none work. Do not invent ICE Retry UI for Amp dial — reuse connect-failed / hop-needed copy under V026.

---

## Multi-hop circuits

When single-hop cannot reach target B, immediate relay R1 may subcontract upstream R2 within a hop cap. Consumer A still scores and pays **R1 only**.

Full protocol and session model: [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md). Bridge score integration: [RELAY_SCOPE.md](../p2p-mesh/RELAY_SCOPE.md).

---

## Maturity summary

| Capability | Maturity | Notes |
|------------|----------|-------|
| Peer address book (L1) | Shipped | Amp-era book / upsert on bootstrap/connect |
| Advertised listen set (L2) | Shipped | ch0 + dial-back / UPnP-derived addrs |
| Circuit PeerId dial (L3) | Shipped | Single-hop; circuit fallback in SoftMigrate path |
| Amp Coordinated Punch (L3.25) | Planned | H009, [HOLE_PUNCH.md](HOLE_PUNCH.md) |
| Multi-hop circuit (L3.5) | Planned | H008, N024 — parallel to punch |
| SoftMigrate consume only (L4) | In progress | Drop reliance on empty contact ma as only signal |
| Directory / DHT dial assist (L5) | Planned | Still closed-set for media hops |

Detail and checkboxes: [PHASES.md](PHASES.md), [CURRENT_STATE.md](CURRENT_STATE.md).

---

## References (decisions)

| Topic | ADR |
|-------|-----|
| Implementation in Amp mesh | H001 |
| Publish > punch > circuit > fail | H002 |
| Contacts as cache only | H003 |
| No WebRTC / app STUN | H004 |
| Circuit + billing modes | H005 |
| Mobile never hosts | H006 (updated — default Client; N025 call-scoped listen) |
| No `call_hop_addrs` product path | H007 |
| Multi-hop circuit chains | H008 |
| Amp Coordinated Punch | H009 |

Mesh cross-refs: N014 (contact-first), N022 (mesh investment), N024 (brokered relay). Call cross-ref: V026. Amp: D10, A017, A026, A027.
