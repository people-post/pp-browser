# Live broadcast — multi-hop media tree; BroadcastRpcCodec + CallSessionKind::Broadcast landed

**Status:** Spec / ADR — discovery locked ([B007](DECISIONS.md#b007--recursive-whitelist-ladder-discovery-admit-or-redirect)); domain ladder + L1 tip field + ticket arm + BroadcastRpcCodec + CallSessionKind::Broadcast + AmpBroadcastTransport handlers started  
**Stack ADRs:** [B001–B007](DECISIONS.md)  
**Product home:** [DESIGN.md](DESIGN.md) (announce + live planes)  
**Program spine:** [PROGRAM.md](PROGRAM.md) **Spine F**  
**Related:** [MEDIA_RELAY_ATTACH](../p2p-mesh/MEDIA_RELAY_ATTACH.md), [HOST_RECEIVE_POLICY](../p2p-av-calls/HOST_RECEIVE_POLICY.md), [RELAY_SCOPE](../p2p-mesh/RELAY_SCOPE.md), [MULTI_HOP_CIRCUIT](../media-hop-reachability/MULTI_HOP_CIRCUIT.md)

## Problem

Spine C live join uses **one** blind `media_relay` hop (contact/seed). Publisher uplink is already “encrypt once”; hop egress still scales as **~N × bitrate**. Massive audiences melt the seed / first-tier hop.

Circuit multi-hop ([MULTI_HOP_CIRCUIT.md](../media-hop-reachability/MULTI_HOP_CIRCUIT.md)) only helps **reach** a single SFU. It does **not** multiply media capacity.

## Goal

Distribute **already-sealed** realtime frames through a **degree-capped tree of blind SFUs** so seed / PreferLocal root egress stays ~`degree × bitrate`, while audience size grows with depth. Audience **finds** capacity via a **recursive whitelist ladder** ([B007](DECISIONS.md#b007--recursive-whitelist-ladder-discovery-admit-or-redirect)) — not a central leaf directory.

## Non-goals

| Non-goal | Why |
|----------|-----|
| Multi-SFU trees for **group calls** | Calls stay soft-max ~16 / one hop ([V007](../p2p-av-calls/DECISIONS.md#v007--participant-cap-16-soft-engineering-floor-8)); tree is **broadcast-only** |
| Circuit legs as media fan-out | R1/R2 remain reachability brokers; media copies only at `media_relay` nodes |
| Per-viewer media encrypt | Would destroy tree copy and uplink ([V004](../p2p-av-calls/DECISIONS.md#v004--shared-call-media-key-not-group-n-ciphertext)) |
| Cleartext media “because public” | AEAD is cheap vs topology; hops stay blind and opportunistic |
| New L4 kind | Still **rpc** (join/ticket/redirect/tree control) + **realtime** datagram fan-out |
| Open helper market in v1 tree | Whitelisted / scoped relays only (`help_media`) |
| Publisher tracks every leaf / viewer placement | Tips list L1 only; deeper placement is hop-local ([B007](DECISIONS.md#b007--recursive-whitelist-ladder-discovery-admit-or-redirect)) |
| SoftMigrate mass roster for audience | Tip → ticket → admit-or-redirect ([B001](DECISIONS.md#b001--broadcast-is-not-a-large-group-call)) |

## Topology

```text
Publisher ──1× uplink──► Root / L0 (PreferLocal Node or publisher-facing hop)
                              │  whitelist ∩ online (tip hints these L1s only)
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
            L1a             L1b             L1c     ← help_media
              │               │
         ┌────┴────┐          …
         ▼         ▼
        L2        L2   …  ──► settle where a free viewer slot exists
```

| Role | Who | Duty |
|------|-----|------|
| **Publisher** | Show owner PeerId | One encrypted uplink; mints session key + join tickets; tip lists **immediate** online whitelist only |
| **Hop / relay** (`help_media`) | Whitelisted durable Nodes | Blind copy upstream→downstream; **same** admit-or-redirect policy as publisher toward viewers; own child whitelist |
| **Viewer** | Audience | Contact an L1 hint → follow redirects until admitted |

**Depth** grows under config caps; **degree** is the hard per-hop fan-out knob (suggested start: 8–16).

## Recursive ladder discovery (B007)

### Separation of planes

| Plane | Carries | Does not carry |
|-------|---------|----------------|
| **Announce tip / heartbeat** | `join_handle`, program ids, **L1 PeerIds** (publisher whitelist ∩ online) | Full tree, leaf maps, media bytes |
| **Join ticket (rpc)** | Viewer-bound media key grant, program/media epoch, expiry | Mandatory leaf assignment (optional soft hint only) |
| **Admit / redirect (rpc)** | Admit ok, or redirect PeerIds + budget/path stamp | Media frames |
| **Realtime** | Opaque sealed frames | Discovery |

### Viewer path — admit or redirect

```text
1. Tip: join_handle + online L1 PeerIds (publisher whitelist)
2. Viewer obtains ticket from publisher (key grant) — once per join/epoch
3. Viewer dials an L1 (scope affinity + jitter among hints)
4. Hop checks ticket + slots:
     free viewer slot → ATTACH (subscriber)
     else             → REDIRECT to hop’s whitelist ∩ online children
                        (capacity-weighted, RELAY_SCOPE affinity, jitter)
5. Viewer retries at redirect target with same ticket; decrement redirect_budget
6. Repeat until attach or budget exhausted → soft fail / re-tip L1 pick
```

Every hop runs **the same algorithm** the publisher uses at L1: local whitelist, local capacity, no global leaf directory.

**Root overflow:** discouraged. When L1 is full, redirect **down** (create depth), do not accumulate viewers on the publisher hop.

**Loops:** redirect messages carry `redirect_budget` and a short path/epoch stamp; refuse cycles.

### Relay path — slot win + ladder demotion

When a new **whitelist relay** wants a child slot under a parent that is at degree / slot pressure:

```text
Parent prefers relay-child over viewer (or lower-priority) occupants
  → demote one or more “piped” viewers one rung down
  → ideally redirect them onto the new relay (grace window, same ticket)
  → new relay wins the parent slot and absorbs demoted load
```

| Rule | Policy |
|------|--------|
| Who may win | Must be on **this parent’s** `help_media` whitelist (and optional publisher program grant) |
| Whom to demote first | Viewers before other relays; never demote in a way that orphans media without redirect |
| UX | Graceful redirect, not hard program kick |
| Anti-churn | Rate-limit slot wins / demotions per parent; hysteresis on “full” |
| Sticky | Prefer sticky viewer→hop while `tree_epoch` / media epoch unchanged when capacity allows |

### Whitelist model (v1)

- **Per-hop allowlist:** each parent decides which PeerIds it will parent as relay-children or (for publisher) advertise as L1.
- **Optional publisher grant:** signed “PeerId R may relay program P until T” checked in addition to local allowlist (hybrid abuse control).
- **Online filter:** tip and redirects only name currently dialable/online candidates.
- **No open market** in v1 ([B005](DECISIONS.md#b005--viewers-settle-at-capacity-leaves-relays-are-help_media-nodes)).

### Compared to alternatives

| Approach | Verdict |
|----------|---------|
| **Recursive ladder (this doc)** | **Default** — tip-small, PreferLocal-friendly, matches `help_media` |
| Coordinator assigns leaf on every ticket | Optional later for ops LB; not required for B1 |
| Viewer picks from open leaf ads | Deferred — herd/stale/abuse |

## Relationship to circuit multi-hop

```text
Viewer ──(optional circuit)──► chosen hop ──tree──► … ──► root ◄── Publisher
         reachability only         media copies
```

Circuit only helps **dial** the PeerId returned by tip or redirect. It does not discover capacity.

## Crypto (keep)

Reuse call AEAD family; change **lifecycle**, not the seal model.

| Rule | Call (V004) | Broadcast tree |
|------|-------------|----------------|
| Frame seal | One AEAD per AU; hop copies ciphertext | **Same** |
| Key count | One shared key / epoch | **One session key** / show (or long epoch) |
| Rotate | On every leave | **Not** on viewer leave; rotate on end / revoke / kick-ban epoch |
| Key delivery | Pairwise wrap to ≤16 | **Join ticket** (publisher-signed); valid at any hop in epoch |
| Hop keys | Never | **Never** |
| Hop payload | Opaque ciphertext | **Required** ([B003](DECISIONS.md#b003--keep-encrypt-once-aead-for-broadcast)) |

### Join ticket (sketch)

```text
Viewer discovers tip (join_handle + L1 hints)
  → RequestJoin(program_id, viewer_peer_id)  // publisher or authorized issuer
  → Ticket { program_id, viewer_peer_id, key_wrap | key_id, exp, media_epoch } signed by publisher
  → Contact L1 → admit or redirect (B007) with same ticket
  → Decrypt under session key
```

Ticket proves **authorization + key**; it does **not** hard-bind the viewer to one leaf. Optional soft `leaf_hint` is advisory only.

## Tree / hop control plane

Own in **p2p-mesh** policy + thin broadcast helpers (not SoftMigrate):

1. **Relay eligibility** — durable Node, `help_media`, capacity ad, scope per [RELAY_SCOPE](../p2p-mesh/RELAY_SCOPE.md).
2. **Child whitelist** — per-hop allowlist (+ optional publisher grant).
3. **Admit-or-redirect** — viewer path ([§ Recursive ladder](#recursive-ladder-discovery-b007)).
4. **Slot win / demotion** — relay path; rate-limited.
5. **Degree / depth caps** — config; refuse over limit ([HOST_RECEIVE_POLICY](../p2p-av-calls/HOST_RECEIVE_POLICY.md) family).
6. **Repair** — parent death → children reparent or viewers re-enter at L1 tip hints; bump `tree_epoch` only when needed.
7. **Budgets** — per-hop A↑/A↓ / ceiling; audio ≫ video_lo; never shed audio for video.

Publisher may publish a compact **digest** on announce (`root` / `tree_epoch`, not full membership) so clients know when to refresh L1 hints.

## Session shape vs calls

| Concern | Group call | Broadcast |
|---------|------------|-----------|
| Product | Mutual / invite grid | One-to-many watch |
| Pickup | Ringtone / Accept | Notifications + banner ([DESIGN](DESIGN.md#product-pickup-ux--not-call-ringing)) |
| Topology | One SFU | **Tree of SFUs** |
| Placement | SoftMigrate / hop pick | **Admit-or-redirect ladder** ([B007](DECISIONS.md#b007--recursive-whitelist-ladder-discovery-admit-or-redirect)) |
| SoftMigrate | Yes (N≥3) | **No** |
| Roster | Full participants | Publisher (+ optional panel); viewers are subscribers |
| Cap | ~8–16 | Tree degree × depth (ops + config) |

## Phased delivery

See [PHASES.md](PHASES.md) **B0–B3** and [PROGRAM.md](PROGRAM.md) **Spine F**.

| Phase | Outcome |
|-------|---------|
| **B0** | Single hop + stable session key + ticket join (Spine C completion) |
| **B1** | 2-tier tree + **L1 tip hints + admit-or-redirect + slot-win demotion** (first scale win) |
| **B2** | Depth >2, richer scope ranking, reparent, capacity ads, redirect budgets hardened |
| **B3** | Multi-root / regional / paid overflow — still blind ladder tree |

**B1** is the first design that answers seed / first-tier pressure under the locked discovery rule.

## Hard-lab

Horizon **N-HARD-MULTI-HOP-MEDIA** remains **off** for group calls. For broadcast, after B1: tree of ≥2 `media_relay` nodes, L1 tip hints, redirect when full, slot-win demotion of a viewer onto a new relay. Do not overload call SoftMigrate fixtures.

## One-line summary

**Live scale = degree-capped blind `media_relay` tree; tip names online L1 whitelist only; ticket carries the key; each hop admits or redirects down; new relays can win slots and push piped viewers one rung down; circuit only reaches the chosen hop; calls stay single-hop.**


## Implementation note (code split)

Live-announce **arm/accept** lives in `feature/calls/BroadcastSessionCoordinator` (owned by `CallSessionManager::Broadcast()`). SoftMigrate early-skips Broadcast sessions; `AcceptInvite` refuses them so audience joins cannot fall onto the call SoftMigrate path.
