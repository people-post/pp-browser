# Libp2p node roles — decisions

## N001 — Desktop node default / mobile always client

**Date:** 2026-07-26  
**Decision:** Exactly two **roles**: **client** and **node**. Desktop effective role is **node** when `node_enabled` is true (default). Mobile is always **client**; `node_enabled` is ignored on mobile.  
**Rationale:** Desktops can host infrastructure; mobiles must stay outbound-only for battery and OS limits.

## N002 — Seed multiaddr IP + 443 + PeerId (no DNS)

**Date:** 2026-07-26  
**Decision:** Fixed bootstrap multiaddr is `/ip4/3.208.41.58/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR`. IP only — no DNS multiaddrs in v1.  
**Rationale:** Deterministic Brief seed without DNS resolution failures; PeerId pins identity.

## N003 — Desktop listen 40123; seed on 443

**Date:** 2026-07-26  
**Decision:** Desktop node listen remains `/ip4/0.0.0.0/tcp/40123`. Seed uses TCP **443**. Do not collapse desktop onto 443.  
**Rationale:** Avoid colliding with local IPFS / other daemons on common ports; seed 443 is firewall-friendly for clients dialing out.

## N004 — Client = no host→listen

**Date:** 2026-07-26  
**Decision:** Effective client role must not call `host->listen` (no private bind workaround in n1).  
**Rationale:** Clear mobile power win and unambiguous role semantics; outbound dials only.

## N005 — Capability protocols deferred; no half-enabled service UI in n1

**Date:** 2026-07-26  
**Decision:** n1 ships role + bootstrap + listen + master Network toggle only. Ship each capability flag **and** its checkbox in the same phase the protocol works (n2+). Pricing UI ships with the first **billable** capability (N010). Do not expose inert checkboxes.  
**Rationale:** Avoid toggles that do nothing; circuit-relay is absent from the fork; economy features need real meters first.

## N006 — Do not use public IPFS bootstrap.libp2p.io

**Date:** 2026-07-26  
**Decision:** Bootstrap only from the Brief seed (and later our own peers). Never default to public IPFS bootstrap lists.  
**Rationale:** Privacy, control, and independence from the public IPFS network.

## N007 — TCP 443 on seed is libp2p transport (≠ HTTPS Brief API)

**Date:** 2026-07-26  
**Decision:** Seed `/tcp/443` is the **libp2p** transport port. It is distinct from HTTPS Brief API endpoints used for HTTP relay/directory/registration.  
**Rationale:** Firewall-friendly outbound for clients without conflating TLS HTTP with multiplexed libp2p.

## N008 — Node = ecosystem infrastructure (mutual help)

**Date:** 2026-07-26  
**Updated:** 2026-07-26 (monetization detail moved to N010)  
**Decision:** **Node** means hosting Brief/pp-browser **infrastructure** for other peers — not only inbound chat sockets. Capability surface includes (over time) DHT, circuit relay, message / audio / video relay, blockchain node, and optional paid-jobs marketplace. Capabilities attach to the **node** role; they do not create new roles. How nodes get paid is **N010**, not “a paid-jobs capability that stands in for settle.”  
**Rationale:** Cooperative mesh where desktops voluntarily supply capacity; clients stay light.

## N009 — Role + capability checkboxes (not capability-only, not binary-only forever)

**Date:** 2026-07-26  
**Decision:** Product model is **role + capability checkboxes**. Role (`node_enabled`) answers “do I host?” Capabilities answer “which services do I run?” while role is Node. UI: master **Help the network** toggle; nested checkboxes appear as each capability ships. Effective service *C* requires `role == Node && C_enabled`. Turning role off forces Client behavior regardless of stored capability flags. Rejected alternatives: (a) forever all-or-nothing Node with no per-service choice; (b) capability-only with no master role. Optional later: presets that set checkbox bundles — still the same model.  
**Rationale:** Users need fine control (e.g. DHT yes, video no) without inventing many modes; mobile stays always-Client.

## N010 — Monetization: per-capability pricing primary; paid jobs secondary

**Date:** 2026-07-26  
**Decision:**

1. **Primary:** Billable capabilities — especially **message / audio / video relay** — each get a **pricing policy**: `volunteer` (free) or `paid` (rate + meter + **on-chain settlement**). Pricing is nested under the capability, not a third role and not a fake “settle” capability.
2. **Secondary:** **Accept paid jobs** is an optional later **marketplace** for discrete tasks. It complements relay pricing; it must **not** be treated as the only or primary way nodes charge others.
3. DHT / circuit relay may remain volunteer longer. Blockchain node provides settlement/identity rails; running a chain node is separate from advertising paid relay rates.
4. Clients may **pay** as consumers; they never host pricing.

**Rationale:** Product intent is “relays may charge other users and settle on chain.” A jobs board is useful but different (continuous infra vs one-off work). Agents must not collapse both into `accept_paid_jobs` alone.
