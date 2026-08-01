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

**Status:** Accepted  
**Decision:** Circuit may enable dial to hop PeerId; quote/billing stay on **media_relay hop PeerId**. Prefer contact then seed bridges (N014). Evolve custom circuit toward PeerId-friendly semantics (L3).  
**Rationale:** Clients need a path without target public IP.  
**Alternatives:** Fail without circuit.

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
