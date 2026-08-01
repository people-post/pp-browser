# Media hop reachability — decisions

Call: [V026](../p2p-av-calls/DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking). Mesh: [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup). Fork notes: [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md).

---

## H001 — Separate project; implementation in libp2p

**Status:** Accepted (updated 2026-07-31)  
**Decision:** This project owns the **program and consume contract**. **Implementation of reachability** lives in the **vendored libp2p fork** and `src/libp2p/integration/` (Identify, addr book, DialBack, circuit, later hole punch). Calls/mesh **consume** dialability; they do not reimplement NAT traversal.  
**Rationale:** One peer stack (N022/V026); app ICE-alikes diverge and rot.  
**Alternatives:** Forever SoftMigrate-only multiaddr glue (rejected — H007).

---

## H002 — Publish in-stack > circuit > fail

**Status:** Accepted (updated 2026-07-31)  
**Decision:** Prefer **stack address book + Reachable ads**, then **circuit**, then SoftMigrate failure. No product dependency on mid-call addr gather.  
**Rationale:** Matches libp2p-native dial; circuit is TURN-analogue for PeerId paths.  
**Alternatives:** App `call_hop_addrs` primary (rejected).

---

## H003 — Contacts may mirror stack addrs (cache only)

**Status:** Accepted  
**Decision:** Contact `multiaddrs` remain a **TTL UX/cache** optionally filled from the stack — not the source of truth for dial. PeerId is identity.  
**Rationale:** SoftMigrate already reads contacts; truth should move to host peerstore.  
**Alternatives:** Contacts-only forever.

---

## H004 — No WebRTC / no app STUN for hops

**Status:** Accepted  
**Decision:** No WebRTC ICE/STUN for hop dial. Observed addrs via **DialBack / Identify / UPnP** inside mesh/libp2p.  
**Rationale:** V026; one stack.  
**Alternatives:** libjuice STUN for advertise only (rejected as product path).

---

## H005 — Circuit last resort; bill media hop

**Status:** Accepted (updated 2026-08-01 — brokered path via N024)  
**Decision:** Circuit may enable dial to hop PeerId. Prefer contact then seed bridges (N014). Evolve custom circuit toward PeerId-friendly semantics (L3).

**Billing — two attach modes:**

| Mode | When | Payer / quote |
|------|------|----------------|
| **Direct attach** | A dials **`media_relay` hop B** directly (stack dialable; no broker) | **A pays B** — N019 quote/ceiling on B ([V022](../p2p-av-calls/DECISIONS.md)) |
| **Brokered attach** | A uses **immediate relay R1** for path and/or media (multi-hop, partition, or R1-as-service) | **A pays R1 only** — bundled circuit + subcontracted media + SLA ([N024](../p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker)) |

Direct attach remains the simple volunteer / friend-SFU path. Brokered attach is the unified commercial face when R1 orchestrates reachability and downstream `media_relay` capacity.

**Rationale:** Clients need a path without target public IP; friend hops should stay one quote with B; brokered paths need one payer and one SLA owner.  
**Alternatives:** Fail without circuit; always bill B even through R1 (rejected for broker UX — see N024).

---

## H006 — Mobile Client never hosts

**Status:** Accepted  
**Decision:** Mobile never listens / never hosts `media_relay` / never publishes hop listen addrs as a Node.  
**Rationale:** Role model.  
**Alternatives:** Mobile temporary hop (rejected).

---

## H007 — No app-layer hop candidate exchange as product path

**Status:** Accepted  
**Date:** 2026-07-31  
**Decision:** Do **not** ship or reintroduce **`call_hop_addrs`** (or similar call-signaling multiaddr gather) as the durable hop reachability design. Uncommitted prototypes were removed. Temporary dogfood hacks need an explicit ADR if ever revived.  
**Rationale:** Duplicates what Identify/peerstore/circuit should do; fights “reachability inside libp2p.”  
**Alternatives:** Keep thin gather until L1 (rejected — prefer document gap + stack work).

---

## H008 — Multi-hop circuit chains (planned)

**Status:** Accepted (plan only — **not implemented**)  
**Date:** 2026-08-01  
**Updated:** 2026-08-01 (hop cap config; brokered pricing N024)  
**Decision:** Custom circuit (`/pp-browser/circuit-relay`) must evolve from **single-hop** to **multi-hop transitive paths** (A → immediate relay **R1** → optional upstream **R2+** → target **B**). Consumer **selects and contracts with R1 only** ([N024](../p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker)). R1 chooses upstream relays and (when brokered) subcontracted **`media_relay`** capacity on B.

**Hop limit:** Configurable **`circuit_relay.max_hops`** (default **3** relay legs on path). The stack must **not** hardcode a protocol maximum — honor whatever limit ops/config sets (including &gt; 3). Loop detection and per-hop admission still apply.

**Rationale:** L3 landed single-hop bridge; real meshes need transitive reachability. Broker model lets R1 own path + media economics and SLA.  
**Alternatives:** Retry only alternate single relays (rejected); consumer pays every hop (rejected); fixed compile-time hop max (rejected).  
**Spec:** [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md). **Phase:** [L3.5](PHASES.md#l35--multi-hop-circuit-v2).
