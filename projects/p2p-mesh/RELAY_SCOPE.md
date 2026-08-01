# Relay scope & domain bridging

**Project:** [p2p-mesh](README.md)  
**ADRs:** [N023](DECISIONS.md#n023--relay-scope-and-domain-bridging-not-geography-tiers) (scope model), [N014/N020](DECISIONS.md) (pick + incentives)  
**Stack dialability:** [media-hop-reachability](../media-hop-reachability/) (in-libp2p; **not** this doc)  
**Networking:** [NETWORKING.md](../../docs/architecture/NETWORKING.md)

## One-line goal

Clients **maximize delivery** by escalating relay choice across **nested connectivity domains** (link → site → social → org → public), with **minimal user input** — scope is inferred from reachability signals and the contact graph, not chosen as “local vs global tier.”

## Problem statement

Peers are not on one flat network. They sit in **nested partitions**:

| Partition (human name) | Technical cause | Symptom |
|------------------------|-----------------|---------|
| **Link / site** | LAN, Bluetooth island, office gateway | Low RTT subset; many peers `OutboundOnly` |
| **Egress / country** | Firewall, ISP filter, captive portal | Seed or global peers unreachable (`Blocked`, `seed_dial_ok == false`) |
| **Global** | Public internet | Org seed, curated directory, paid overflow |

A relay’s value is **how many useful partition boundaries it crosses**, not map geography.

**Sub-problems:**

1. **Partitioning** — Who can dial whom directly?
2. **Bridging** — Who connects two partitions (gateway, seed, in-domain super-peer)?
3. **Selection** — Which hop/store/bridge for this message or call, without sybil/min-price failure?

Relay types differ by delivery semantics ([DESIGN.md](DESIGN.md)):

| Service | Semantics | Scope relevance |
|---------|-----------|-----------------|
| **Circuit relay** | Live stream bridge | Prefer narrow scope (same site) before org seed |
| **Media relay** | Live blind fan-out | Same; N019 quote + N020 scorer |
| **Message relay** (peer, later) | Store-and-forward | Island/gateway queues; HTTP Brief remains durability anchor |
| **HTTP Brief relay** | Org inbox | Global/org backstop for messages |

## Solution model

### Relay scope (narrow → wide)

Scopes are **machine tags** on relay advertisements — inferred automatically; users set policy only at capability + pricing level (N009/N010).

| Scope | Meaning | Typical inference |
|-------|---------|-------------------|
| **`link`** | Same L2/L3 neighborhood | mDNS, same /24, Bluetooth pairing |
| **`site`** | Same household/org LAN behind one gateway | RFC1918 peers + shared household tag |
| **`social`** | Contacts / trusted graph | `MeshHopAffinity::Contact` |
| **`org`** | Brief seed + curated directory | Bootstrap peers, signed directory entries |
| **`public`** | Strangers (paid / regulated) | Reachable public IP + rate card + opt-in |

**Not geo tiers:** “Country” is modeled as an **egress partition** — peers who share bootstrap failure but can still reach each other. Selection uses **bridge score**, not country IP databases.

**`link` / `site` vs closed set (N020 short term):** Scope tags are **ordering and score dimensions among already-eligible peers**, not permission to hop via arbitrary LAN strangers. For `media_relay` / circuit short term, eligible peers remain **contacts ∪ household ∪ org seed** only (household tag deferred — v1 treats “contact on same LAN/subnet” as `link`/`site`). mDNS may inform inference later; it must not widen the feasible set until mid-term policy says so.

**Household / trusted:** Named in N014/N020 feasible set; maps to **`site`** (+ social). No household field on contacts yet — ship v1 as contacts-on-same-subnet only.

### Topology roles (emergent, not user job titles)

| Role | Behavior | Who becomes one |
|------|----------|-----------------|
| **Participant relay** | Helps known peers | Default desktop Node (volunteer) |
| **Gateway relay** | Bridges site → wider internet | `Reachable` node with `OutboundOnly` LAN neighbors |
| **Infrastructure relay** | Stable org capacity | `pp-node` on `:443` |
| **Island store** | Queue until gateway syncs | Bluetooth/LAN store-and-forward (future) |

### Consumer algorithm: escalate scope

**Two loops (do not confuse with rejected N014 stage machine):**

1. **Outer:** scope bands `link → site → social → org` (short term; **`public` empty** until N020 mid-term directory/paid class).
2. **Inner:** N020 **filter → score → quote → attach → re-pick** within each band.

Hardcoded contacts→seed→public **without** scoring/re-pick remains rejected (N020/V023). Scope escalation adds a narrow→wide **eligibility band**, not a fixed winner per band.

Generalizes N020 for all relay kinds:

```mermaid
flowchart TB
  need[Need relay path]
  need --> esc[For scope in link .. public]
  esc --> filt[Filter: advertises scope + eligible + dialable + quality floor]
  filt --> score[Score: affinity + bridge + latency + capacity + price + reputation]
  score --> quote[Quote + payer accept N019]
  quote --> try[Attach / deliver]
  try -->|ok| done[Done]
  try -->|fail| cd[Cooldown exclude; next candidate or next scope]
  cd --> esc
  cd --> fb[Fallback: HTTP Brief messages / clear UX for calls]
```

**Short-term consumer mask (N020):** `link | site | social | org` only — skip `public` until curated directory ships.

**Rules:**

- Prefer **narrowest scope that works** — same-LAN contact before org seed.
- On partition (`Blocked` / seed unreachable), boost **bridge score** within `social` before widening to `org` (alternate org relays / directory mid-term — not blind retry of the same unreachable seed).
- Never pure `min(price)` (N020).
- Messages: HTTP Brief inbox remains final durability fallback (N017).

### Provider algorithm: auto-cap scope

Minimal UX: **Help the network** + capability checkboxes + nested pricing (N009/N010). Scope caps derive from signals:

| Signal | Provider cap (default volunteer) |
|--------|----------------------------------|
| `node_enabled == false` | Advertise nothing |
| Always (Node) | `social` |
| Same-LAN peers detected | Also `link` / `site` |
| `Reachability == Reachable` | May offer `org` (ops profile); `public` only if paid + opt-in |
| `Reachability == OutboundOnly` | Refuse strangers; serve `social` / `link` aggressively |
| `Reachability == Blocked` | Do not promise wide scope; island queue only if implemented |

Extend admission policies: **`serve_scope_mask`** is authoritative for stranger dialers; `prefer_contacts_only` retained for config/UI sync only (ns1).

### Bridge score (partition escape)

When global path fails, rank **immediate relay R1** candidates by:

```text
bridge_score = dialable_from_me
             × r1_reaches_target
             × recent_bridge_success
             × affinity_bonus
```

Where **`r1_reaches_target`** is true when R1 can complete a path to B:

- **Today (single-hop):** R1 can **direct-dial** B (or seed reachable from R1 when B is org-scoped).
- **Planned (multi-hop, [H008](../media-hop-reachability/DECISIONS.md#h008--multi-hop-circuit-chains-planned)):** R1 advertises or proves reachability to B **directly or via subcontract** (upstream R2 within hop cap). Consumer A still scores **R1 only** — not R2 ([N024](DECISIONS.md#n024--circuit-pricing-pay-immediate-relay-only)).

No user selects “country tier.” The client discovers: *seed unreachable from me; contact Alice (R1) still reaches the target.*

### Multi-hop bridge score

See [MULTI_HOP_CIRCUIT.md](../media-hop-reachability/MULTI_HOP_CIRCUIT.md). Summary:

| Side | Algorithm |
|------|-----------|
| **Consumer (A)** | Escalate scope → score **R1** → quote/accept with R1 → opaque tunnel to B |
| **Provider (R1)** | If no direct dial to B, inner pick **R2** (scope, margin, capacity) → nested circuit |

**Pricing:** A pays **R1 only** for circuit; R1 optimizes upstream so margin stays positive when paid.

### Ecosystem health by scope

| Scope | Supply | Abuse risk | Regulation |
|-------|--------|------------|------------|
| `link` / `site` | Mutual aid, low cost | Low | Volunteer default |
| `social` | Reciprocity | Medium | Contact admission + quality floor |
| `org` | Ops / mission | Medium | Seed SLAs; optional paid overflow |
| `public` | Revenue potential | High | Paid + bonds + anti-dumping + concentration penalty (N020 mid/long) |

**Healthy defaults:** volunteer Node serves `link + social`, not `public`. Wider scope unlocks with reachable hardware + paid rate card (or org `pp-node`).

## Ownership (do not blur)

| Layer | Owns |
|-------|------|
| **[media-hop-reachability](../media-hop-reachability/)** | In-stack **dialability**: peerstore, Identify, UPnP/dial-back, circuit evolution, `IsPeerDialable` |
| **This doc / p2p-mesh policy** | **Who** may relay, **which scope**, bridge score, escalate order, admission, quotes, settle |
| **p2p-av-calls** | SoftMigrate **consumes** ranked hops + dialability |
| **HTTP Brief** | Message inbox durability (global/org backstop) |

Do **not** expand media-hop-reachability to cover scope routing or incentives — that splits ownership locked in [H001](../media-hop-reachability/DECISIONS.md#h001--separate-project-implementation-in-libp2p).

## Code anchors (ns1 landed → ns2 open)

| Area | State |
|------|-------|
| `RelayScope.h` | Scope bit flags + `RelayAdmissionAllowsDialer` (header-only; libp2p-safe) |
| `CandidateRelayScopes` | Contact → social/site/link; OrgSeed → org; same-/24 v1 (needs **bound** listen, not `0.0.0.0`) |
| `RankMediaHopsEscalating` | Media consumer pick in `CallTopologyController` |
| `OrderCircuitHops` | Circuit immediate-relay order (nf); ns2: escalate + bridge score |
| Multi-hop circuit v2 | **Not implemented** — [MULTI_HOP_CIRCUIT.md](../media-hop-reachability/MULTI_HOP_CIRCUIT.md), [N024](DECISIONS.md#n024--circuit-pricing-pay-immediate-relay-only) |
| `ProviderServeScopeMask` + `serve_scope_mask` | Admission on circuit + media relay |
| `ApplyMeshAdmissionPolicies` | Reachability-aware stranger limit; re-run on probe update |
| `prefer_contacts_only` on admission structs | Legacy field; **admission uses `serve_scope_mask`** |
| Bridge score / Identify ads / Me → Network preset | ns2+ |

## Optional advanced setting (not required for v1)

Single Me → Network control maps to scope mask:

| Label | Mask |
|-------|------|
| Auto (recommended) | Inferred from reachability + capabilities |
| Contacts & nearby | `link \| site \| social` |
| Wider network | + `org`; `public` only with paid (mid-term) |

Default is **auto** from reachability + capabilities.

## Phasing

See [PHASES.md § ns](PHASES.md#ns--relay-scope-and-domain-bridging-n023). Depends on **nr/nu** signals and **nf/n4-media** scorer; does not block [media-hop-reachability L1–L3](../media-hop-reachability/PHASES.md).

## Related

- [DESIGN.md § Relay path preference](DESIGN.md#relay-path-preference-n014--n020)
- [DECISIONS.md N023](DECISIONS.md#n023--relay-scope-and-domain-bridging-not-geography-tiers)
- [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md) — HTTP Brief fallback
