# P2P mesh — decisions

## N001 — Desktop node default / mobile always client

**Date:** 2026-07-26  
**Decision:** Exactly two **roles**: **client** and **node**. Desktop effective role is **node** when `node_enabled` is true (default). Mobile is always **client**; `node_enabled` is ignored on mobile.  
**Rationale:** Desktops can host infrastructure; mobiles must stay outbound-only for battery and OS limits.

## N002 — Seed multiaddr IP + 443 + PeerId (no DNS)

**Date:** 2026-07-26  
**Decision:** Fixed bootstrap multiaddr is `/ip4/3.208.41.58/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR`. IP only — no DNS multiaddrs in v1.  
**Rationale:** Deterministic Brief seed without DNS resolution failures; PeerId pins identity.

## N003 — Desktop listen 18517; seed on 443

**Date:** 2026-07-26  
**Updated:** 2026-07-26 (default port **18517**; was 40123)  
**Decision:** Desktop / in-app Node preferred listen is `/ip4/0.0.0.0/tcp/18517`. Seed / org `pp-node` uses TCP **443**. Do not collapse desktop onto 443. Busy-port behavior is **N016**.  
**Rationale:** App-specific high port avoids IPFS **4001** and other common daemons; 18517 is the Brief desktop default. Seed 443 stays firewall-friendly for clients dialing out.

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

## N011 — Separate `pp-node` binary for org / headless servers

**Date:** 2026-07-26  
**Decision:** Dedicated infrastructure (Brief org seeds, datacenter nodes, systemd/Docker daemons) runs **`pp-node`**, a headless binary that links the shared node runtime (libp2p host, bootstrap, later capabilities) **without** SDL/RmlUi. End-user apps remain **`pp-browser`** with in-process Client/Node (N009). Rejected as the production server path: `pp-browser --headless` / `--node-only` alone (GUI dependency weight, PIN/window coupling, poor ops images). Optional GUI `--node-only` may exist later for local dogfood only. Org seed listen stays **tcp/443**; in-app desktop Node preferred listen stays **18517** (N003). One core, two entrypoints — do not maintain a second networking stack.  
**Rationale:** Seed `3.208.41.58:443` and future org nodes need a small, non-interactive process; user desktops need a GUI. Same mesh protocols and PeerId model for both.

## N012 — Reachability status + guided network help

**Date:** 2026-07-26  
**Decision:** When Node participation matters, surface a **reachability status** (Reachable / Outbound only / Blocked / Unknown) based on measurable signals (private vs public listen IP, dial to seed, later dial-back / AutoNAT) — not definitive “you are behind a firewall” claims. Me → Network shows a Connection card; soft banner + guided sheet teach **port forwarding** (outbound-only + private IP), firewall allowlisting (blocked), and always offer **relay / skip**. Clients are not nagged to port-forward. `pp-node` gets ops status, not consumer copy. Cheap detection + help UI is phase **nr**; full AutoNAT / hole punch come later.  
**Rationale:** Mutual-help nodes fail silently behind NAT; users need to learn what is wrong and what they can do without feeling broken or blamed.

## N013 — Prefer IPv6 + UPnP/NAT-PMP before manual port forward

**Date:** 2026-07-26  
**Decision:** To become **Reachable**, try **IPv6 advertisement** and **UPnP / NAT-PMP / PCP** mapping before relying on the N012 manual port-forward checklist. UI: auto-try or one-tap “Open port on router”; on failure, fall back to guided manual forward. Org `pp-node` on public IPs need not use UPnP. Phase **nu**.  
**Rationale:** Most users never complete router configuration; IPv6 and UPnP convert far more Nodes to inbound-reachable.

## N014 — Contact-first relay preference (ask friends; serve friends)

**Date:** 2026-07-26  
**Decision:** Relay hop selection (circuit / message / media) uses a **priority policy**: prefer **contacts** (and household/trusted tags), then org seed, then public volunteer/paid relays, then HTTP Brief fallback. When **hosting** a relay, prefer capacity for **contacts/friends** before strangers (especially volunteer desktop Nodes). This is policy on capabilities — not a new role. Friends must opt in via Node + capability; never coerce. Phase **nf**, with or right after circuit-relay (**n3**).  
**Rationale:** Users want to ask people they know for routing and to help those people first; public paid relays are a backstop, not the default social path.

## N015 — Delivery order: reachability and circuit before DHT

**Date:** 2026-07-26  
**Decision:** Preferred ship order is **n1 → np (incl. dial-back) → nr → nu (IPv6/UPnP) → n3 (circuit-relay) → nf (contact-first) → n4 (billable relays + pricing) → directory/reputation → n2 (DHT) → chain/jobs/Home Node**. DHT remains in scope but is not the next feature after n1 by default.  
**Rationale:** Users feel “Node works” when they are reachable and can hop via trusted peers; Kademlia helps discovery later but does not unblock NAT or friend routing.

## N016 — Listen port busy: desktop fallback + persist; `pp-node` fail loud

**Date:** 2026-07-26  
**Decision:**

1. **Preferred desktop port** is **18517** (N003) in `listen_multiaddr` `/ip4/0.0.0.0/tcp/18517`.
2. **`pp-browser` (Node):** If bind on the preferred port fails, try a small consecutive range (e.g. **18517–18526**), then optionally an OS ephemeral port if the range is exhausted. **Persist** the successfully bound port into config (`listen_multiaddr`). Surface the **actual** listen port in Me → Network / Connection card and in any port-forward / UPnP UI — never coach “forward 18517” if the host bound another port.
3. **`pp-node` (ops):** By default **fail loudly** if the configured listen port cannot bind (especially **443**). Do not silently hop ports (firewall/systemd expectations). An explicit opt-in flag (e.g. `--listen-fallback`) may allow range fallback later; not the default.
4. Today’s code returns a generic listen failure and continues without libp2p — n1/np must replace that with N016 behavior + clear user/ops errors.

**Rationale:** Fixed preferred port keeps docs and UPnP simple; fallback avoids silent “Node on but dead” when 18517 is taken; ops seeds must not drift off 443 without the operator noticing.
