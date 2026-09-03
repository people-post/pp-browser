# Hard lab — phases

Ordering only. Ladder detail: [HARD_LAB.md](../../packaging/pp-node/HARD_LAB.md). Status: [CURRENT_STATE.md](CURRENT_STATE.md).

## h0 — Docs + ownership

- [x] DESIGN / DECISIONS / PHASES / CURRENT_STATE / README
- [x] Canonical ladder in `packaging/pp-node/HARD_LAB.md`
- [x] Purpose catalog + Gate F in [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md)
- [x] Cross-links from IMAGE_SMOKE / TESTING / docs map / projects index

## h1 — Wave 1 forced hop (clean)

- [ ] `docker-compose.hard-lab.yml` (net-a / net-hop / net-b; hop dual-homed; no A↔B)
- [ ] Scenario runner(s) for **N-HARD-FORCE**
- [ ] **B-HARD-CALL** via `pp-call-probe` (or equivalent) in peer containers
- [ ] **B-HARD-MSG+CALL** on forced topo
- [ ] `pp_local_test.sh run --suite hard` entry (Wave 1 only)
- [ ] Inventory status → scaffold in TEST_STRATEGY

## h2 — Wave 2 path quality

- [ ] `tc netem` / `tbf` profile hooks (`lossy`, `asym`, `bw`)
- [ ] **N-HARD-LOSSY**, **N-HARD-ASYM**; optional **N-HARD-BW**
- [ ] Document flake/retry policy from first evidence

## h3 — Wave 3 discovery

- [ ] **N-HARD-STALE-ADDR**, **N-HARD-SEED-ONLY**
- [ ] **N-HARD-DIR** when directory lab hooks exist
- [ ] **N-HARD-DHT** extending `pp_node_dht_smoke` into hard nets
- [ ] **N-ADMIT-HARD** (closes deploy-profile gap on forced topo)

## h4 — Wave 4 multi-hop (blocked on L3.5)

Depends on [media-hop-reachability L3.5](../media-hop-reachability/PHASES.md#l35--multi-hop-circuit-v2) / mesh ns3.

- [ ] Loopback multi-host protocol green first (prefer before Docker)
- [ ] **N-HARD-MHOP-PATH**, **B-HARD-MHOP-CALL**
- [ ] **N-HARD-MHOP-LOOP**, **N-HARD-MHOP-REPICK**
- [ ] Add **N-HARD-MHOP-PATH** to sparse release set

## h5 — Wave 5 NAT shapes

- [ ] Explicit product policy for hairpin fallback (**N-HARD-HAIRPIN**)
- [ ] **N-HARD-CGNAT-ISH** SNAT lab
- [ ] Optional **N-HARD-UPNP** / **N-HARD-V6** / **N-HARD-PATH-MIGRATE**
- [ ] Hole punch remains non-goal until stack ships it

## h6 — Wave 6 product stress

- [ ] **B-HARD-TEARDOWN**, **B-HARD-CONFLICT**
- [ ] **N-HARD-MIX**, **N-HARD-SOAK**
- [ ] **B-HARD-MOBILE-LISTEN** if N025 lab profile is worth automating

## h7 — Horizons

Placeholders only (HARD_LAB Wave 7). No checkboxes until product triggers fire.
