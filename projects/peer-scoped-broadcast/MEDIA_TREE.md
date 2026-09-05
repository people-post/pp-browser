# Live broadcast — multi-hop media tree

**Status:** Spec / ADR only — **not implemented**  
**Stack ADRs:** [B001–B006](DECISIONS.md)  
**Product home:** [DESIGN.md](DESIGN.md) (announce + live planes)  
**Program spine:** [PROGRAM.md](PROGRAM.md) **Spine F**  
**Related:** [MEDIA_RELAY_ATTACH](../p2p-mesh/MEDIA_RELAY_ATTACH.md), [HOST_RECEIVE_POLICY](../p2p-av-calls/HOST_RECEIVE_POLICY.md), [RELAY_SCOPE](../p2p-mesh/RELAY_SCOPE.md), [MULTI_HOP_CIRCUIT](../media-hop-reachability/MULTI_HOP_CIRCUIT.md)

## Problem

Spine C live join uses **one** blind `media_relay` hop (contact/seed). Publisher uplink is already “encrypt once”; hop egress still scales as **~N × bitrate**. Massive audiences melt the seed / first-tier hop.

Circuit multi-hop ([MULTI_HOP_CIRCUIT.md](../media-hop-reachability/MULTI_HOP_CIRCUIT.md)) only helps **reach** a single SFU. It does **not** multiply media capacity.

## Goal

Distribute **already-sealed** realtime frames through a **degree-capped tree of blind SFUs** so seed / PreferLocal root egress stays ~`degree × bitrate`, while audience size grows with depth.

## Non-goals

| Non-goal | Why |
|----------|-----|
| Multi-SFU trees for **group calls** | Calls stay soft-max ~16 / one hop ([V007](../p2p-av-calls/DECISIONS.md#v007--participant-cap-16-soft-engineering-floor-8)); tree is **broadcast-only** |
| Circuit legs as media fan-out | R1/R2 remain reachability brokers; media copies only at `media_relay` nodes |
| Per-viewer media encrypt | Would destroy tree copy and uplink ([V004](../p2p-av-calls/DECISIONS.md#v004--shared-call-media-key-not-group-n-ciphertext)) |
| Cleartext media “because public” | AEAD is cheap vs topology; hops stay blind and opportunistic |
| New L4 kind | Still **rpc** (join/ticket/tree control) + **realtime** datagram fan-out |
| Open helper market in v1 tree | Whitelisted / scoped relays only (`help_media`) |

## Topology

```text
Publisher ──1× uplink──► Root SFU (PreferLocal Node or org seed)
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
           Tier-1          Tier-1          Tier-1
         (help_media)    (contact/site)   (org child)
              │               │
         ┌────┴────┐          …
         ▼         ▼
       Leaf      Leaf  ──► viewers attach here (prefer leaves)
```

| Role | Who | Media duty |
|------|-----|------------|
| **Publisher** | Show owner PeerId | One encrypted uplink to **root**; mints session key + join tickets |
| **Root** | PreferLocal durable Node, else ranked seed | Fan-out to ≤`degree` children (relays and/or viewers) |
| **Relay** (`help_media`) | Whitelisted durable Nodes | Subscribe upstream; fan-out downstream; blind copy |
| **Viewer** | Audience | Attach to **nearest eligible leaf** (not the seed when a leaf is available) |

**Depth** grows under config caps; **degree** is the hard per-hop fan-out knob (suggested start: 8–16).

## Relationship to circuit multi-hop

```text
Viewer ──(optional circuit R1→R2)──► Leaf SFU ──tree──► … ──► Root ◄── Publisher
         reachability only              media copies
```

- Circuit: how a NAT’d viewer **dials** their chosen leaf.  
- Tree: how sealed frames **propagate** from root toward leaves.  

[MULTI_HOP_CIRCUIT](../media-hop-reachability/MULTI_HOP_CIRCUIT.md) non-goal of multi-hop **media bit paths** remains for **calls**. Broadcast **explicitly allows** multi-SFU media under this doc ([B002](DECISIONS.md#b002--broadcast-allows-multi-sfu-media-tree-calls-do-not)).

## Crypto (keep)

Reuse call AEAD family; change **lifecycle**, not the seal model.

| Rule | Call (V004) | Broadcast tree |
|------|-------------|----------------|
| Frame seal | One AEAD per AU; hop copies ciphertext | **Same** |
| Key count | One shared key / epoch | **One session key** / show (or long epoch) |
| Rotate | On every leave | **Not** on viewer leave; rotate on end / revoke / kick-ban epoch |
| Key delivery | Pairwise wrap to ≤16 | **Join ticket** (publisher-signed grant → session key or key wrap) |
| Hop keys | Never | **Never** (blindness preserved) |
| Hop payload | Opaque ciphertext | **Required** — every hop must copy opaque sealed frames ([B003](DECISIONS.md#b003--keep-encrypt-once-aead-for-broadcast)) |

Encryption is mandatory. Do not add a cleartext media path; hops that cannot forward opaque blobs are not eligible as tree relays.

### Join ticket (sketch)

```text
Viewer discovers tip (join_handle / program_id)
  → RequestJoin(program_id, viewer_peer_id)  // rpc to publisher or authorized issuer
  → Ticket { program_id, viewer_peer_id, key_wrap | key_id, exp, leaf_hint? } signed by publisher
  → Attach to leaf with ticket + program bind
  → Decrypt under session key
```

Late joiners get the **current** epoch key via ticket; mid-show rekey is rare (kick/ban) and uses a new epoch + tip event (bypasses heartbeat floor once — see [DESIGN § Live re-announce](DESIGN.md#live-re-announce-heartbeat)).

## Tree control plane

Own in **p2p-mesh** policy + thin broadcast coordinator (not SoftMigrate):

1. **Relay eligibility** — durable Node, `help_media` for this publisher/program, capacity ad, scope (link → site → social → org) per [RELAY_SCOPE](../p2p-mesh/RELAY_SCOPE.md).
2. **Parent selection** — new relay picks parent with free child slots, lowest depth, best scope affinity; PreferLocal root when publisher Node hosts.
3. **Viewer attach** — prefer leaf with free A↓ / child slots in viewer’s scope; fall back toward root only if no leaf dialable.
4. **Degree / depth caps** — config; refuse attach over limit (same family as [HOST_RECEIVE_POLICY](../p2p-av-calls/HOST_RECEIVE_POLICY.md)).
5. **Repair** — parent death → children reparent; publisher tip may advertise `root_peer_id` + optional `tree_epoch`; do not rebuild whole tree on one leaf failure.
6. **Budgets** — per-hop A↑/A↓ / ceiling; audio ≫ video_lo; never shed audio for video.

Publisher/coordinator may publish a compact **tree digest** on the announce plane (root id, tree_epoch, not full membership) so viewers refresh leaf hints without a second gossip mesh.

## Session shape vs calls

| Concern | Group call | Broadcast |
|---------|------------|-----------|
| Product | Mutual / invite grid | One-to-many watch |
| Pickup | Ringtone / Accept | Notifications + banner ([DESIGN](DESIGN.md#product-pickup-ux--not-call-ringing)) |
| Topology | One SFU | **Tree of SFUs** |
| SoftMigrate | Yes (N≥3) | **No** — join is tip→ticket→leaf attach |
| Roster | Full participants | Publisher (+ optional panel later); viewers are subscribers |
| Cap | ~8–16 | Tree degree × depth (ops + config) |

Spine C may reuse call **session types** for attach convenience; tree membership and SoftMigrate stay out of that path ([B001](DECISIONS.md#b001--broadcast-is-not-a-large-group-call)).

## Phased delivery

See [PHASES.md](PHASES.md) **B0–B3** and [PROGRAM.md](PROGRAM.md) **Spine F**.

| Phase | Outcome |
|-------|---------|
| **B0** | Broadcast-shaped single hop + stable session key + ticket join (Spine C completion) |
| **B1** | Degree-capped **2-tier** media (root + children); viewers prefer leaves |
| **B2** | Depth >2, relay election, reparent, capacity ads |
| **B3** | Multi-root / regional seeds, paid overflow — still blind tree |

**B1** is the first design that answers seed / first-tier pressure.

## Hard-lab

Horizon **N-HARD-MULTI-HOP-MEDIA** remains **off** for group calls. For broadcast, add a broadcast-scoped scenario only after B1 lands (tree of two `media_relay` nodes + N viewers). Do not overload call SoftMigrate fixtures.

## One-line summary

**Live scale = degree-capped tree of blind `media_relay` nodes copying encrypt-once ciphertext; viewers attach to leaves; circuit only reaches the leaf; calls stay single-hop.**
