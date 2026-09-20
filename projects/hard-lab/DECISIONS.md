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
**Decision:** Hard-lab “close to dogfood” means **CallStack Invite→Accept→Bridge StartSfu→Leave** over real Amp on forced/CGNAT topo, with a first-class **dirty-book** profile (register peer private advertise MA / optional dial-fail→backoff before circuit). Full GUI stays out of the lab (HL001). Wave 6 product gate: **B-HARD-CALL-NAT-DIRTY** (reach under poisoned dial book) then **B-HARD-CALL-NAT-STACK** (product invite wire + media).  
**Rationale:** Two-network dogfood fails on GUI dial-book + Bridge Ensure, not on “can Amp carry one frame via hop.” Dirty-book + CallStack cover that without Rml/SDL flake. Promote policy misses to gtest ([TESTING.md](../../docs/architecture/TESTING.md)).  
**Alternatives:** Full `pp-browser` in Docker (rejected — cost/flake); only thin Amp duplex forever (rejected — misses product glue).
