# Hard lab — decisions

## HL001 — Thin clients + profile matrix harness

**Status:** Accepted (2026-09-03)  
**Decision:** Hard lab uses **thin probes** (`pp-node-probe` / `pp-call-probe`), not full GUI. One compose family with orthogonal **topo × link × disco** profiles; sparse CI release set — not a combinatorial matrix job.  
**Rationale:** Matches Gate C (thin client) and TESTING doctrine (smoke = namespace/path truth; GUI is sparse). Keeps flake surface debuggable.  
**Alternatives:** Full GUI E2E in Docker (rejected — cost/flake); one mega-compose per scenario (rejected — undebuggable).

## HL002 — Ladder lives in packaging; delivery in projects

**Status:** Accepted (2026-09-03)  
**Decision:** Canonical scenario ladder and topology live in [`packaging/pp-node/HARD_LAB.md`](../../packaging/pp-node/HARD_LAB.md). Purpose IDs and cadence live in [`docs/ops/TEST_STRATEGY.md`](../../docs/ops/TEST_STRATEGY.md). This project folder tracks phases/status only — do not fork a second editable ladder.  
**Rationale:** One editable home per fact ([TESTING.md](../../docs/architecture/TESTING.md) doc-homes rule).

## HL003 — Wave 1 before impairment / multi-hop / NAT shapes

**Status:** Accepted (2026-09-03)  
**Decision:** Implement **forced hop on clean links** before netem, DHT, multi-hop, or CGNAT-ish profiles. Multi-hop hard-lab scenarios are blocked on media-hop **L3.5**.  
**Rationale:** Biggest deployment gap vs relay-smoke is “A cannot reach B except via hop.” Impairments on a broken topology are noise.

## HL004 — E2E closeness = CallStack + Amp on netns (not GUI)

**Status:** Accepted (2026-09-20)  
**Decision:** Hard-lab “close to dogfood” means **CallStack Invite→Accept→Bridge StartSfu→Leave** over real Amp on forced/CGNAT topo, with a first-class **dirty-book** profile (register peer private advertise MA / optional dial-fail→backoff before circuit). Full GUI stays out of the lab (HL001). Wave 6 product gate: **B-HARD-CALL-NAT-DIRTY** (reach under poisoned dial book) then **B-HARD-CALL-NAT-STACK** (`CallUiBackend` StartCall/Accept/Leave + real `OnMeshServicesStarted` Wire; `BindTestMediaPath` is gtest-only).
**Rationale:** Two-network dogfood fails on GUI dial-book + Bridge Ensure, not on “can Amp carry one frame via hop.” Dirty-book + CallStack cover that without Rml/SDL flake. Promote policy misses to gtest ([TESTING.md](../../docs/architecture/TESTING.md)).
**Alternatives:** Full `pp-browser` in Docker (rejected — cost/flake); only thin Amp duplex forever (rejected — misses product glue).
**Update (2026-09-26):** the DIRTY gate is **superseded by B-HARD-CALL-NAT-COLD-DIRTY** — same dirty book + forced dial miss, but through the product `PeerReachCoordinator` (product stack, signaling via `/share`, no pre-built peer link) instead of a probe-local reach copy that also re-warmed the hop. It found a call-media glare bug the probe copy masked.

---

## HL005 — CGNAT gateways: explicit NAT mapping, firewalled WAN input

**Status:** Accepted (2026-09-28, call-path-resilience k7)  
**Decision:** The Wave 5 gateways drop unsolicited WAN packets at INPUT (as real routers do) and run an explicit mapping mode: **symmetric** (`MASQUERADE --random-fully`, default — a fresh public port per destination, punching can never land) or **cone** (port-preserving, endpoint-independent mapping — punchable). Scripts flip modes and blackhole the gateway↔gateway path at runtime (`pp_hard_cgnat_set_nat`, `pp_hard_cgnat_block_p2p`); `pp_hard_cgnat_ensure_up` restores symmetric + unblocked. Relay-shaped phases run symmetric; punch phases (UPGRADE, PUNCH) run cone.  
**Rationale:** The old gateways accepted unsolicited WAN input, so a peer's early punch packet was confirmed in conntrack as an inbound flow; our host's outbound mapping to that peer then clashed with it and MASQUERADE picked another port — every punch failed, by an artefact no real NAT has. The lab looked like "hard NAT" by accident. Making the mapping explicit keeps the relay-only phases deterministic and gives punch-dependent behaviour (upgrade to direct, dual-dial election, failover back to the relay) a real stage.  
**Alternatives rejected:** WAN delay (netem on the gateways) so simultaneous open wins the race — the clash still fires whenever one end's burst starts a one-way delay late; the product would pass or fail on scheduling jitter.
