# Hard lab — forced-hop / NAT / impairment design

**Status:** Wave 1 scaffold green (**N-HARD-FORCE** + **B-HARD-CALL** + **B-HARD-MSG+CALL**) — compose + probes + `--suite hard`; Wave 2+ open  
**Tier:** ops / Tier C (multi-netns smoke)  
**Doctrine:** [TESTING.md](../../docs/architecture/TESTING.md)  
**Purposes / CI:** [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md) (`N-HARD-*`, `B-HARD-*`)  
**Delivery tracking:** [projects/hard-lab/](../../projects/hard-lab/)  
**Complements:** [IMAGE_SMOKE.md](IMAGE_SMOKE.md) (L0–L2 on host-published hop — does **not** force A↛B)

Loopback compose and ADP loss matrices own policy and wire semantics. This lab owns **namespace truth**: peers that cannot talk directly must discover and use hop paths under optional link impairment.

---

## Goal

Simulate a **hard local deployment** so thin peers must:

1. Fail (or never attempt) direct A↔B
2. Find a usable path via hop (seed / book / directory / DHT — per scenario)
3. Complete **circuit** (chat/control) and/or **media_relay** (call frames)
4. Optionally survive lossy links, stale addrs, multi-hop brokers, and product lifecycle stress

**Clients:** `pp-node-probe` / `pp-call-probe` (thin). Full GUI is out of scope.

---

## Relation to relay-smoke (L0–L2)

| Lab | Shape | Proves |
|-----|-------|--------|
| `docker-compose.relay-smoke.yml` | Hop in Docker; probes on **host** | Packaged hop + outside dial (N-REACH / N-FANOUT) |
| **Hard lab** (planned) | A, hop, B in **isolated nets**; no A↔B route | Forced hop + discovery + impairments |

Do **not** run hard-lab compose and relay-smoke on conflicting published ports without remapping. Prefer a separate compose family: `docker-compose.hard-lab.yml` (name TBD at implement time).

---

## Topology (Wave 1 minimum)

```text
        [net-a]              [net-hop]             [net-b]
     peer-a (thin) ──x──     hop (pp-node)     ──x── peer-b (thin)
           \                    ↑                    /
            \_____ only UDP/Amp to hop allowed _____/
```

- Three Docker networks (or equivalent netns).
- Hop attached to `net-a` and `net-b` (dual-homed) **or** a small router container; peers are **not** on a shared network.
- Firewall / absent route ensures **direct A↔B fails**.
- Status HTTP may stay on an ops network or published to the driver host for `/healthz` only.

### Multi-hop extension (Wave 4)

```text
A ──► R1 ──x──► B     (R1 cannot dial B)
A ──► R1 ──► R2 ──► B (R1 brokers upstream; A configures R1 only)
```

See [MULTI_HOP_CIRCUIT.md](../../projects/media-hop-reachability/MULTI_HOP_CIRCUIT.md). Prefer loopback multi-host for protocol correctness first; hard lab proves netns + packaging of the same shape.

---

## Orthogonal profile axes

Scenarios are **profiles** on one harness, not one-off mega-composes.

| Axis | Values (initial) |
|------|------------------|
| `topo` | `force` (single hop), `mhop` (R1+R2), later `cgnat`, `hairpin` |
| `link` | `clean`, `lossy`, `asym`, `bw` |
| `disco` | `static` (seed/hop known), `stale`, `seed-only`, `dir`, `dht` |

**Not every cell is a CI job.** Sparse [release set](#sparse-release-set) below.

### Link profiles (`tc netem` / `tbf` on veth — implement later)

| Profile | Intent (starting knobs; tune with evidence) |
|---------|-----------------------------------------------|
| `clean` | No impairment |
| `lossy` | ~2–5% loss, ~50–100 ms RTT, light jitter on A↔hop and/or B↔hop |
| `asym` | One direction or one peer outbound-only / blackhole as designed |
| `bw` | `tbf` cap on media path |

Flake policy: topology-force scenarios should be **deterministic**; netem may allow **one retry**.

---

## Scenario ladder (waves)

Wave 0 is prerequisite (already largely landed). Hard lab **starts** at Wave 1.

### Wave 0 — Prerequisites (not hard-lab work)

| Owns | Evidence |
|------|----------|
| Loopback partition / circuit+media | `circuit_*_compose_test`, `loopback_partition_fixture` |
| L0–L2 / `N-FANOUT` | [IMAGE_SMOKE.md](IMAGE_SMOKE.md) |
| `B-CALL-HOP` / msg-call-hop | thin probes + `pp_local_test.sh` |
| ADP loss / path migrate | L1 / A-INT matrices |

### Wave 1 — Forced hop (clean links)

| # | ID | Pass |
|---|----|------|
| 1 | **N-HARD-FORCE** | Circuit payload A→B via hop; media quote+attach+≥1 frame each way; direct A↔B fails |
| 2 | **B-HARD-CALL** | Invite→InCall→Leave for D seconds via media_relay; path marked hop |
| 3 | **B-HARD-MSG+CALL** | Chat during/after call (circuit chat + media hop) on forced topo |

**First implementation milestone:** #1 then #2–3. Driver sketch: `pp_local_test.sh run --suite hard`.

### Wave 2 — Path quality

| # | ID | Profile | Pass |
|---|----|---------|------|
| 4 | **N-HARD-LOSSY** | `link=lossy` | Wave 1 completes within budget; no hop fatal |
| 5 | **N-HARD-ASYM** | `link=asym` | Completes via circuit / dial-back / observed-addr story (no ICE) |
| 5b | **N-HARD-BW** | `link=bw` | Call stays up or fails clean; chat not starved forever (soft SLO later) |

Compose existing **N-CHAOS** onto Wave 1 topo when useful (hop kill/pause); do not invent a parallel chaos ID.

### Wave 3 — Discovery & address truth

| # | ID | Situation | Pass |
|---|----|-----------|------|
| 6 | **N-HARD-STALE-ADDR** | Peer advertises unusable listen | Direct via stale fails; book/hop path succeeds |
| 7 | **N-HARD-SEED-ONLY** | PeerId + seed/hop only (no peer multiaddr) | Dial-by-PeerId via hop/circuit succeeds |
| 8 | **N-HARD-DIR** | Directory returns PeerId (+ optional stale hint) | Resolve → dial → Wave 1 success |
| 9 | **N-HARD-DHT** | Islands + DHT escape hint | Lookup → usable path → circuit/media |
| 10 | **N-ADMIT-HARD** | Stranger vs allowlist on forced topo | Expected accept/reject + status counters |

Order: 6 → 7 → 8 → 9. Admission (#10) may parallel 6–7. Extends today’s partial **N-ADMIT**.

### Wave 4 — Multi-hop circuit (after L3.5 / ns3)

| # | ID | Pass |
|---|----|------|
| 11 | **N-HARD-MHOP-PATH** | A↔R1 OK, R1↛B, R1↔R2↔B; opaque session via R1; A never configures R2 |
| 12 | **B-HARD-MHOP-CALL** | Invite→Leave on brokered path; A sees/pays R1 only (policy assert soft at first) |
| 13 | **N-HARD-MHOP-LOOP** | Cycle / `max_hops` exceeded → reject cleanly; no hang |
| 14 | **N-HARD-MHOP-REPICK** | Kill/pause R2 → fail clean or R1 re-pick within documented bounds |

**Non-goal:** multi-SFU media bitpaths (one subcontracted media hop — [MULTI_HOP_CIRCUIT.md](../../projects/media-hop-reachability/MULTI_HOP_CIRCUIT.md)).

### Wave 5 — NAT shapes (weekly / manual heavy)

| # | ID | Shape | Pass / caveat |
|---|----|-------|---------------|
| 15 | **N-HARD-CGNAT-ISH** | Separate SNAT gateways; no inbound without hop | Forced hop works; not every carrier CGNAT quirk |
| 16 | **N-HARD-HAIRPIN** | Both peers behind same NAT; hairpin may fail | Direct **or** hop fallback — **product policy must be explicit** before coding |
| 17 | **N-HARD-UPNP** | UPnP/PCP mock or router lab | Reachability → Reachable; optional direct (org public hop may skip) |
| 18 | **N-HARD-V6** | IPv6-only island ↔ dual-stack hop | Only if v6 listen is product-real |
| 19 | **N-HARD-PATH-MIGRATE** | Change egress mid-call | Survive or clean renegotiate; Tier A/B own wire semantics |

**Hole punch:** `non-goal` until the stack ships it.

### Wave 6 — Product stress on hard topology

Same Wave 1 nets; harder product criteria (reuse B-* meanings):

| # | ID | Combines | Pass |
|---|----|----------|------|
| 20 | **B-HARD-TEARDOWN** | B-TEARDOWN × forced hop | K cycles; no orphan listen; K+1 works |
| 21 | **B-HARD-CONFLICT** | B-CONFLICT × forced hop | Busy / end-and-accept across nets |
| 22 | **N-HARD-MIX** | Allowlisted parallel children | Each child keeps criteria; hop healthy |
| 23 | **N-HARD-SOAK** | Wave 1 + light loss + churn T | 0 fatal; still accepts; FD/RSS bounded |
| 24 | **B-HARD-MOBILE-LISTEN** | N025 ephemeral listen profile | Dial-by-PeerId during “foreground Wi‑Fi” lab profile |

Phones / real ISP NAT = **manual dogfood**, not automated Wave 6.

### Wave 7 — Horizons (placeholders)

| # | ID | Trigger |
|---|----|---------|
| 25 | **N-HARD-LEDGER-DIR** | On-chain name → PeerId is product path |
| 26 | **N-HARD-PAID-BROKER** | Quote/settle/SLA must be lab-visible |
| 27 | **N-HARD-HOLEPUNCH** | Stack ships hole punch |
| 28 | **N-HARD-MULTI-HOP-MEDIA** | Group calls: still **non-goal**. Broadcast: after peer-scoped-broadcast Spine F / B1 ([MEDIA_TREE.md](../../projects/peer-scoped-broadcast/MEDIA_TREE.md)) |
| 29 | Real multi-operator WAN | Never the only gate |

---

## Sparse release set

Once Wave 1–3 exist, default nightly/pre-release sparse set:

1. `N-HARD-FORCE`
2. `B-HARD-CALL`
3. `B-HARD-MSG+CALL`
4. `N-HARD-LOSSY`
5. `N-HARD-STALE-ADDR`
6. `N-HARD-MHOP-PATH` — **when L3.5 lands**

Everything else: weekly, manual, or on-demand.

---

## Cross-cutting rules

1. One compose family + many scenario scripts/profiles.
2. Binary product outcomes first; capacity curves stay `N-CAP-*`.
3. Do not re-assert codec/SM detail already covered in gtests (`covered-below`).
4. Do not claim “NAT tested” because a hop port is published to the host.
5. When hard lab finds a **policy** bug (not pure netns/packaging), promote a cheaper gtest — [TESTING.md § When a higher tier finds a bug](../../docs/architecture/TESTING.md#when-a-higher-tier-finds-a-bug). Example: nested-chat reachability → `AmpDirectChatCircuitNestedTest`.
6. Pass/fail and cadence for purposes live in [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md); this file owns topology/profiles/ladder detail.

---

## Implementation sketch (carry out later)

```text
packaging/pp-node/docker-compose.hard-lab.yml   # nets + hop + peer-a/b
packaging/pp-node/Dockerfile.hard-peer          # Debian peer sidecar for host probes
scripts/test/pp_hard_force_smoke.sh                  # N-HARD-FORCE runner
scripts/test/pp_local_test.sh run --suite hard       # Wave 1 entry
pp-node-probe --mode bridge-target|bridge-via-hop|media-recv|media-send
```

**Landed (Wave 1):** isolation + circuit/media force + product call + chat-during-call on forced nets.  
**Next:** Wave 2 netem — see [projects/hard-lab/PHASES.md](../../projects/hard-lab/PHASES.md).
