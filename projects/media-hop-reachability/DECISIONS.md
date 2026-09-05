# Media hop reachability — decisions

Call: [V026](../p2p-av-calls/DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking). Mesh: [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup). Amp mesh: [NETWORKING.md](../../docs/architecture/NETWORKING.md), [MESH.md](../../docs/architecture/MESH.md).

---

## H001 — Separate project; implementation in Amp mesh

**Status:** Accepted (updated 2026-09-04 — Amp underlay)  
**Decision:** This project owns the **program and consume contract**. **Implementation of reachability** lives in **Amp + `src/domain/mesh/`** (ch0 addr ads, addr book, DialBack, UPnP, circuit, **Amp Coordinated Punch**). Calls/mesh **consume** dialability; they do not reimplement NAT traversal. In-tree libp2p retains PeerId/crypto helpers only (A017) — not a product Host/DCUtR path.  
**Rationale:** One peer stack (N022/V026/D10); app ICE-alikes diverge and rot.  
**Alternatives:** Forever SoftMigrate-only multiaddr glue (rejected — H007); revive libp2p DCUtR as product punch (rejected — A017).

---

## H002 — Publish in-stack > punch > circuit > fail

**Status:** Accepted (updated 2026-09-04 — insert ACP before circuit)  
**Decision:** Prefer **stack address book + Reachable ads**, then **Amp Coordinated Punch** ([H009](#h009--amp-coordinated-punch-acp)), then **circuit**, then SoftMigrate failure. No product dependency on mid-call addr gather.  
**Rationale:** Direct PeerLinks are cheaper/lower-latency than TURN-analogue circuit when NATs are punchable; circuit remains the PeerId-path fallback.  
**Alternatives:** App `call_hop_addrs` primary (rejected); punch after multi-hop only (rejected — parallel tracks).

---

## H003 — Contacts may mirror stack addrs (cache only)

**Status:** Accepted  
**Decision:** Contact `multiaddrs` remain a **TTL UX/cache** optionally filled from the stack — not the source of truth for dial. PeerId is identity.  
**Rationale:** SoftMigrate already reads contacts; truth should move to host address book.  
**Alternatives:** Contacts-only forever.

---

## H004 — No WebRTC / no app STUN for hops

**Status:** Accepted (updated 2026-09-04 — Amp observed addrs)  
**Decision:** No WebRTC ICE/STUN for hop dial. Observed addrs via **DialBack / ch0 ads / UPnP** (and later punch success) inside Amp mesh.  
**Rationale:** V026; one stack.  
**Alternatives:** libjuice STUN for advertise only (rejected as product path).

---

## H005 — Circuit last resort; bill media hop

**Status:** Accepted (updated 2026-08-01 — brokered path via N024)  
**Decision:** Circuit may enable dial to hop PeerId. Prefer contact then seed bridges (N014). Evolve custom circuit toward PeerId-friendly semantics (L3). Successful **Amp Coordinated Punch** prefers **direct attach** over brokered.

**Billing — two attach modes:**

| Mode | When | Payer / quote |
|------|------|----------------|
| **Direct attach** | A dials **`media_relay` hop B** directly (stack dialable; no broker) | **A pays B** — N019 quote/ceiling on B ([V022](../p2p-av-calls/DECISIONS.md)) |
| **Brokered attach** | A uses **immediate relay R1** for path and/or media (multi-hop, partition, or R1-as-service) | **A pays R1 only** — bundled circuit + subcontracted media + SLA ([N024](../p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker)) |

Direct attach remains the simple volunteer / friend-SFU path. Brokered attach is the unified commercial face when R1 orchestrates reachability and downstream `media_relay` capacity.

**Rationale:** Clients need a path without target public IP; friend hops should stay one quote with B; brokered paths need one payer and one SLA owner.  
**Alternatives:** Fail without circuit; always bill B even through R1 (rejected for broker UX — see N024).

---

## H006 — Mobile default Client; call-scoped listen on Wi‑Fi

**Status:** Accepted (updated 2026-08-01 — **N025**)  
**Decision:** Mobile **defaults** to Client: no always-on listen, no always-on `media_relay`, no Node capability UI.

**Planned exception ([N025](../p2p-mesh/DECISIONS.md#n025--mobile-call-scoped-listen-on-wi-fi-not-full-node)):** **Ephemeral listen** on **Wi‑Fi** during a **foreground call** (and optional later **Wi‑Fi helper** toggle) so peers can dial by PeerId on LAN and in-call mobile hops can attach. Scope and relay admission remain **contacts / in-call only** — not public infrastructure.

Idle background reachability still uses **outbound dial + circuit** (and later punch when an introducer Session exists), not persistent mobile listen.

**Rationale:** Role model + battery; scoped listen fixes LAN PeerId dial without full Node.  
**Alternatives:** Mobile temporary hop with no listen gating (rejected — battery/abuse); full mobile Node (rejected).

---

## H007 — No app-layer hop candidate exchange as product path

**Status:** Accepted (updated 2026-09-04 — Amp stack wording)  
**Date:** 2026-07-31  
**Decision:** Do **not** ship or reintroduce **`call_hop_addrs`** (or similar call-signaling multiaddr gather) as the durable hop reachability design. Uncommitted prototypes were removed. Temporary dogfood hacks need an explicit ADR if ever revived. Candidate exchange for punch belongs **in-stack** via introducer Sessions ([H009](#h009--amp-coordinated-punch-acp)), not call signaling.  
**Rationale:** Duplicates what addr book / punch / circuit should do; fights “reachability inside Amp mesh.”  
**Alternatives:** Keep thin gather until L1 (rejected — prefer document gap + stack work).

---

## H008 — Multi-hop circuit chains (planned)

**Status:** Accepted (plan only — **not implemented**)  
**Date:** 2026-08-01  
**Updated:** 2026-09-04 (parallel to ACP; not a substitute for punch)  
**Decision:** Custom circuit must evolve from **single-hop** to **multi-hop transitive paths** (A → immediate relay **R1** → optional upstream **R2+** → target **B**). Consumer **selects and contracts with R1 only** ([N024](../p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker)). R1 chooses upstream relays and (when brokered) subcontracted **`media_relay`** capacity on B.

**Hop limit:** Configurable **`circuit_relay.max_hops`** (default **3** relay legs on path). The stack must **not** hardcode a protocol maximum — honor whatever limit ops/config sets (including &gt; 3). Loop detection and per-hop admission still apply.

**Parallel to punch:** Multi-hop covers **unpunchable** partitions; [H009](#h009--amp-coordinated-punch-acp) covers **punchable** direct paths. Neither blocks the other.

**Rationale:** L3 landed single-hop bridge; real meshes need transitive reachability. Broker model lets R1 own path + media economics and SLA.  
**Alternatives:** Retry only alternate single relays (rejected); consumer pays every hop (rejected); fixed compile-time hop max (rejected); wait for punch before multi-hop (rejected — different failure modes).  
**Spec:** [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md). **Phase:** [L3.5](PHASES.md#l35--multi-hop-circuit-v2).

---

## H009 — Amp Coordinated Punch (ACP)

**Status:** Accepted (plan only — **not implemented**)  
**Date:** 2026-09-04  
**Decision:** Ship hole punching as **Amp Coordinated Punch**: an introducer that already has authenticated Sessions to both peers exchanges **observed Amp UDP endpoints** and a sync window; both sides simultaneous-dial; first authenticated PeerLink wins under **A026**, with **A027** parent-only teardown for the loser. Prefer seed / Reachable contact / current circuit R1 as introducer (v1). Optional **upgrade-from-circuit** promotes a direct PeerLink then demotes the tunnel.

**Not product paths:** WebRTC/ICE, app STUN, `call_hop_addrs`, libp2p Host DCUtR.

**Preference:** Inserted in [H002](#h002--publish-in-stack--punch--circuit--fail) between publish and circuit.

**Rationale:** After D10/A017 the underlay is Amp UDP; DCUtR-class behavior must be Amp-native. SoftMigrate must not grow a parallel NAT toolkit. Keepalive/`MaybeLearnPath` are not punch.  
**Alternatives:** App ICE gather (rejected — H004/H007); wait forever on UPnP/IPv6 only (rejected — many outbound-only homes); circuit-only forever (rejected — cost/latency); public STUN farm (rejected — second trust plane).  
**Spec:** [HOLE_PUNCH.md](HOLE_PUNCH.md). **Phase:** [L3.25](PHASES.md#l325--amp-coordinated-punch).
