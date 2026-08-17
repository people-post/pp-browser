# Test strategy (tiers, purposes, inventory)

**Tier:** ops

How we stress and qualify **pp-node** (hop services) and **pp-browser** (product actions). Purpose IDs (`N-*`, `B-*`) are the vocabulary for multi-process work. Deploy smoke layers L0–L2 live in [IMAGE_SMOKE.md](../../packaging/pp-node/IMAGE_SMOKE.md).

Related: [BUILD.md](BUILD.md), [CALLS.md](../architecture/CALLS.md), [NETWORKING.md](../architecture/NETWORKING.md).

---

## Principles

1. **Purpose-first** — pick the question, then the cheapest layer that can answer it.
2. **Cost order** — unit → local integration (in-process / loopback) → deploy smoke → multi-node.
3. **Hard filter** — if a failure mode reproduces with in-process loopback ([`loopback_partition_fixture.h`](../../src/base/p2p/tests/loopback_partition_fixture.h)), it does **not** belong in multi-node.
4. **Three kinds of testing** (orthogonal to tiers):

| Kind | Question |
|------|----------|
| **Correctness** | Right outcomes / state transitions |
| **Reliability / soak** | No hang, leak, or deadlock over time |
| **Capacity / SLO** | How much before quality or success rate collapses |

5. **Do not merge** pp-node and pp-browser into one suite with shared pass criteria. Share fixtures where useful; qualify separately.

---

## Tiers

```text
Tier A  Unit              logic, codecs, SMs, QoS rules
Tier B  Local integration real stacks, one machine (loopback / in-process multi-host)
        Deploy smoke      L0/L1 packaged node (IMAGE_SMOKE) — not stress
Tier C  Multi-node        isolation, deploy reachability, multi-process fan-out, capacity, soak, chaos
```

| Layer | Answers | Cost | Flake risk |
|-------|---------|------|------------|
| Tier A | Logic, codecs, SMs, admission, QoS | Lowest | Low |
| Tier B | Real stacks on one machine | Medium | Medium |
| Deploy smoke | Image/env/caps/reach from outside hop | Medium | Low–medium |
| Tier C | Topology, process isolation, capacity, long soak | Highest | Highest |

---

## Product split

| Product | Stress surface |
|---------|----------------|
| **pp-node** | Concurrent clients against hop caps: circuit relay, media relay, dial-back, status HTTP, admission, fan-out QoS |
| **pp-browser** | End-to-end actions: invite → accept → media → leave; messaging; teardown; conflict |

Full GUI is the wrong default stress vehicle. Prefer thin clients / in-process compose; reserve GUI for sparse chrome checks (`B-UI`).

---

## CI ladder

| Gate | Contents |
|------|----------|
| **Every PR** | Tier A + stable Tier B compose set (below) + pp-node L0 if image/packaging touched |
| **Nightly** | Heavier local soak; L1; L2 `N-FANOUT` when stable; modest capacity (small N); `--suite mix` |
| **Weekly / pre-release** | `N-SOAK`, `N-CHAOS`, larger `N-CAP-*`, `B-CALL-HOP` on compose |
| **Manual dogfood** | Real phones, NAT, UPnP — never the only gate |

Capacity tests emit a **curve** (success rate / latency vs N). First qualify “does not crash and recovers”; then tighten SLOs.

---

## Core compose PR set (Tier B)

Keep these **PR-blocking** when `PP_BROWSER_BUILD_TESTS=ON` (desktop). They are the local stand-ins for call/hop correctness:

| Concern | ctest / sources |
|---------|-----------------|
| Direct call-media | `call_media_direct_service_test` — [`src/base/p2p/tests/call_media_direct_service_test.cpp`](../../src/base/p2p/tests/call_media_direct_service_test.cpp) |
| Media relay fan-out | `media_relay_service_test` — [`media_relay_service_test.cpp`](../../src/base/p2p/tests/media_relay_service_test.cpp) |
| Circuit + call-media | `circuit_call_media_compose_test` — [`circuit_call_media_compose_test.cpp`](../../src/base/p2p/tests/circuit_call_media_compose_test.cpp) |
| Circuit + media_relay | `circuit_media_relay_compose_test` — [`circuit_media_relay_compose_test.cpp`](../../src/base/p2p/tests/circuit_media_relay_compose_test.cpp) |
| Circuit bridges | `circuit_relay_service_test` |
| Call phase SM | `call_lifecycle_test` — [`src/feature/messaging/tests/call_lifecycle_test.cpp`](../../src/feature/messaging/tests/call_lifecycle_test.cpp) |

Run (from a configured desktop build tree):

```bash
ctest --test-dir build -R 'CallMediaDirect|MediaRelayService|CircuitCallMedia|CircuitMediaRelay|CircuitRelayService|CallLifecycle|Libp2pDirectChat' --output-on-failure --no-tests=error
```

Exact ctest names follow CMake target naming under `pp_browser_*`; adjust `-R` if a local tree renames targets.

### Local driver (lifecycle + suites)

[`scripts/pp_local_test.sh`](../../scripts/pp_local_test.sh) owns Docker hop **up / stop / clear** and calls the existing smoke scripts. Default hop file: [`docker-compose.relay-smoke.yml`](../../packaging/pp-node/docker-compose.relay-smoke.yml). `run --suite node` restages a **newer desktop `pp-node`** into the hop image when `dist/pp-node/docker/pp-node` is older than `build/src/app/node/pp-node` (avoids muxer failures against a stale packaged hop).

```bash
./scripts/pp_local_test.sh run --suite unit    # core compose ctest (no Docker)
./scripts/pp_local_test.sh run --suite call    # B-CALL-DIRECT thin client
./scripts/pp_local_test.sh run --suite conflict  # B-CONFLICT 3-peer thin client
./scripts/pp_local_test.sh run --suite msg-call  # B-MSG+CALL chat during/after call
./scripts/pp_local_test.sh run --suite node    # L0/L1/N-FANOUT/N-CAP N=4 (starts hop)
./scripts/pp_local_test.sh run --suite cap     # N-CAP-MEDIA sweep + N-CAP-CIRCUIT
./scripts/pp_local_test.sh run --suite soak    # N-SOAK (default 120s; PP_NODE_SOAK_SEC=3600 weekly)
./scripts/pp_local_test.sh run --suite chaos   # N-CHAOS kill-client + restart + pause
./scripts/pp_local_test.sh run --suite call-hop  # B-CALL-HOP thin client via packaged hop
./scripts/pp_local_test.sh run --suite mix     # B-MIX + N-MIX interference (not in all)
./scripts/pp_local_test.sh run                 # unit + call + conflict + msg-call + node (not cap/soak/chaos/call-hop/mix)
./scripts/pp_local_test.sh stop                # compose stop, volume kept
./scripts/pp_local_test.sh clear               # down -v + ready-file
```

`run` leaves the hop up unless `--down`. Package `dist/pp-node/docker` before `up` / `node` (`scripts/pp_node_package_linux.sh all`).

---

## Purpose catalog — pp-node (`N-*`)

| ID | Purpose | Topology | Pass / fail (qualify) | Cadence |
|----|---------|----------|------------------------|---------|
| **N-SMOKE** | Packaged node boots with expected caps | 1 container/binary | `/healthz` + `/status`; caps match env | Release + PR optional |
| **N-REACH** | External peer can use hop | Probe host ≠ hop netns | Media `RequestQuote` + circuit bridge payload | Local dogfood; release optional |
| **N-FANOUT** | Real multi-process fan-out | hop + client-a + client-b | Attach×2, subscribe, ≥1 frame each receiver; no peer mix | Nightly / release-optional |
| **N-ADMIT** | Admission under deploy profile | hop + stranger probe | Expected reject/accept; status counters move | Nightly |
| **N-CAP-MEDIA** | Media hop capacity | 1 hop + N attachers | Success ≥ X% to N₀; graceful degrade; no crash; RSS/FD bounded | Nightly / manual |
| **N-CAP-CIRCUIT** | Circuit bridge capacity | 1 hop + M bridges | Setup ≥ Y%; teardown reclaims capacity | Nightly / manual |
| **N-SOAK** | Long-run stability | hop + churning clients | T hours: 0 fatal; FD/RSS bounded; dials still accepted | Weekly / pre-release |
| **N-CHAOS** | Process/network faults | kill/pause/restart | Survivors recover or fail cleanly; hop accepts new work | Manual / weekly |
| **N-MIX** | Hop interference (existing smokes in parallel) | 1 hop + allowlisted children | Each child keeps its own criteria; hop `/healthz` after | Nightly |

### Inventory (thin)

| ID | Status | Primary evidence |
|----|--------|------------------|
| N-SMOKE | **Covered** | [`scripts/pp_node_image_smoke.sh`](../../scripts/pp_node_image_smoke.sh); release CI L0 |
| N-REACH | **Covered** | [`pp-node-probe`](../../src/app/node/probe/main.cpp); [`scripts/pp_node_relay_smoke.sh`](../../scripts/pp_node_relay_smoke.sh) |
| N-FANOUT | **Covered** (scaffold) | L2: `pp-node-probe --mode media-fanout` + [`scripts/pp_node_fanout_smoke.sh`](../../scripts/pp_node_fanout_smoke.sh); in-process: `media_relay_service_test` |
| N-ADMIT | **Partial** | gtests (contacts-only / call-scoped); no deploy-profile stranger probe |
| N-CAP-MEDIA | **Covered** (soft scaffold) | `pp-node-probe --mode media-cap` sweep (`--attachers 4,8,12,16` or `--sweep 4:16:4`) + p50/p95; [`scripts/pp_node_cap_smoke.sh`](../../scripts/pp_node_cap_smoke.sh); driver `--suite cap`. Soft SLO: 100% attach for N≤**N₀=8** (first curve: hop participant limit 8; N=12/16 degrade). Hop RSS/FD via `docker stats`. |
| N-CAP-CIRCUIT | **Covered** (soft scaffold) | `pp-node-probe --mode circuit-cap` + [`scripts/pp_node_circuit_cap_smoke.sh`](../../scripts/pp_node_circuit_cap_smoke.sh). Soft SLO: 100% for M≤**M₀=4**; larger M informational. In-process: `ConcurrentBridges`. |
| N-SOAK | **Covered** (scaffold) | `pp-node-probe --mode media-soak` + [`scripts/pp_node_soak_smoke.sh`](../../scripts/pp_node_soak_smoke.sh); driver `--suite soak`. Default **120s**; weekly `PP_NODE_SOAK_SEC=3600`. Fail on hop death or 3 consecutive attach failures. |
| N-CHAOS | **Covered** (scaffold) | [`scripts/pp_node_chaos_smoke.sh`](../../scripts/pp_node_chaos_smoke.sh); driver `--suite chaos`. Kill client mid-attach; `docker restart`; pause/unpause. In-flight streams need not survive restart. |
| N-MIX | **Covered** (scaffold) | [`scripts/pp_mix_hop_smoke.sh`](../../scripts/pp_mix_hop_smoke.sh): call-hop×2 ∥ N-FANOUT ∥ circuit-cap **M=2**. Combined load stays under N₀=8. Not chaos / cap sweep / soak. Driver `--suite mix`. |

---

## Purpose catalog — pp-browser (`B-*`)

| ID | Purpose | Topology | Pass / fail | Cadence |
|----|---------|----------|-------------|---------|
| **B-CALL-DIRECT** | Product call path without hop | 2 thin clients / in-process | Invite→InCall→Leave; media ok for D seconds | Nightly (multi-process); PR (in-process) |
| **B-CALL-HOP** | Call via pp-node hop | 2 clients + 1 pp-node | Same; path marked hop/relay | Nightly |
| **B-TEARDOWN** | No stuck listen/media after leave | Repeat K cycles | After K: no orphan listen; call K+1 works | Nightly |
| **B-CONFLICT** | Second invite / end-and-accept | 3 peers | Lifecycle rules hold across processes | Nightly |
| **B-MSG+CALL** | Messaging + call coexistence | 2 peers | Chat during/after call; no stream starvation | Nightly |
| **B-UI** | Chrome/actions only | 1–2 GUI instances | Accept visible; leave clears chrome — not media SLOs | Manual / sparse |
| **B-MIX** | Browser interference (existing smokes in parallel) | independent pairs | Each child keeps its own criteria | Nightly |

### Inventory (thin)

| ID | Status | Primary evidence |
|----|--------|------------------|
| B-CALL-DIRECT | **Partial** | In-process: `call_media_direct_service_test`, `CallMediaKeyStore` Put/Load; multi-process thin client: `pp-call-probe` + [`scripts/pp_call_direct_smoke.sh`](../../scripts/pp_call_direct_smoke.sh); product `CallLibp2pMediaBridge` still untested as a unit |
| B-CALL-HOP | **Covered** (scaffold) | In-process: `circuit_call_media_compose_test`, `circuit_media_relay_compose_test`; multi-process: `pp-call-probe --via-hop` + [`scripts/pp_call_hop_smoke.sh`](../../scripts/pp_call_hop_smoke.sh); driver `--suite call-hop` |
| B-TEARDOWN | **Partial** | `ConnectDetachKCycleNoHang` (direct); `--cycles` on `pp-call-probe` (direct and hop); Detach/timeout/Stop no-hang in services |
| B-CONFLICT | **Covered** (scaffold) | In-process: `CallMediaDirectServiceTest.SecondInboundRejectedThenEndAndAccept`; multi-process: `pp-call-probe --expect busy` + [`scripts/pp_call_conflict_smoke.sh`](../../scripts/pp_call_conflict_smoke.sh); driver `--suite conflict`. Chrome copy still unit-only. |
| B-MSG+CALL | **Covered** (scaffold) | In-process: `Libp2pDirectChatServiceTest.ChatDuringAndAfterCallMedia` (product `/pp-browser/chat/1.0.0`); multi-process: `pp-call-probe --with-chat` + [`scripts/pp_call_msg_smoke.sh`](../../scripts/pp_call_msg_smoke.sh); driver `--suite msg-call`. |
| B-UI | **Covered at unit** | `call_chrome_sync_test`, `call_conflict_copy_test`; GUI E2E manual only |
| B-MIX | **Covered** (scaffold) | [`scripts/pp_mix_browser_smoke.sh`](../../scripts/pp_mix_browser_smoke.sh): call ∥ conflict ∥ msg-call (ports 47100/47120/47130). Driver `--suite mix`. Same-session mix remains B-MSG+CALL; hop-path chat still deferred. |

**Product-glue hole:** `CallLibp2pMediaBridge` / full `CallSessionManager` path between lifecycle and direct media needs dedicated in-process coverage (Tier B) before claiming full product Invite→Leave.

---

## Soft designs — N-SOAK / N-CHAOS

Automated harnesses (not PR-blocking):

### N-SOAK

- Topology: packaged hop + churning attach/detach (`--mode media-soak --churn 4`)
- Local default: **T = 120s**. Weekly / pre-release: `PP_NODE_SOAK_SEC=3600`
- Pass: 0 process fatals; hop still accepts attach; fail on hop death or 3 consecutive attach failures
- Tighten later: attach success rate, p95 quote latency, RSS/FD sawtooth bound

### N-CHAOS

| Fault | Expected |
|-------|----------|
| Kill client mid-attach | Hop accepts a new client afterward |
| `docker restart` hop | L0 recovers; L1 re-run green |
| Pause hop (~10s) then unpause | Clients fail cleanly or recover; hop accepts new work |

In-flight streams need not survive restart. In-process Partial coverage remains in Stop/Abort/Detach/corrupt-frame gtests. **N-ADMIT** stays later (gtest policy; no deploy-profile stranger probe).

---

## Soft designs — interference mix (`--suite mix`)

Nightly, not PR-blocking, **not** in `all`. Parallel **allowlisted** existing smokes; each child keeps its own pass/fail. Mix pass = all children pass (hop phase also requires `/healthz`). Logs name the failing child.

| Recipe | Children | Notes |
|--------|----------|--------|
| **B-MIX** | `pp_call_direct_smoke.sh` ∥ `pp_call_conflict_smoke.sh` ∥ `pp_call_msg_smoke.sh` | No Docker. Listen ports already distinct. |
| **N-MIX** | `pp_call_hop_smoke.sh` (2 cycles) ∥ `pp_node_fanout_smoke.sh` ∥ `pp_node_circuit_cap_smoke.sh` (M=2) | One hop. Combined load under N₀=8. |

**Do not mix** with N-CHAOS, N-CAP sweep, or N-SOAK (shared hop restart / participant limit). This is contention, not same-session protocol mix (hop-path `--with-chat` still later).

---

## Soft designs — Phase 4 browser E2E (Gate C = thin client)

**Decision:** Option A — thin client (`pp-call-probe`), not full GUI.

| Step | Status |
|------|--------|
| B-CALL-DIRECT multi-process | **Scaffold** — `pp-call-probe` + `pp_call_direct_smoke.sh` |
| B-CALL-HOP | **Scaffold** — `pp-call-probe --via-hop` + `pp_call_hop_smoke.sh`; `--cycles` teardown on hop path |
| B-TEARDOWN K-cycle multi-process | Partial via `--cycles` on offerer (direct and hop) |
| B-CONFLICT | **Scaffold** — 3-peer thin client `pp_call_conflict_smoke.sh`; in-process `SecondInboundRejectedThenEndAndAccept` |
| B-MSG+CALL | **Scaffold** — chat during/after call (`pp_call_msg_smoke.sh` + `ChatDuringAndAfterCallMedia`) |
| B-UI | Manual / unit chrome only |

---

## Replan gates

Later work is intentionally underspecified until evidence exists:

| Gate | When | Decide |
|------|------|--------|
| **A** | After this inventory signed off | Phase 1 (Tier B glue) vs Phase 2 (`N-FANOUT`) order — **done:** Phase 1 then N-FANOUT |
| **B** | After `N-FANOUT` green | `N-ADMIT` vs `N-CAP-*` vs browser `B-CALL-HOP` — **done:** soft N-CAP-MEDIA next |
| **C** | Before browser multi-process E2E | Thin client (preferred) vs full GUI — **done:** Option A thin client |
| **D** | After first N-CAP curve | First local sweep (`--suite cap`, 2026-08-17): N=4/8 **100%**; N=12 **66.7%**; N=16 **50%** (hop `session participant limit` at 8). **N₀ = 8** (100% required); 12/16 stay informational. Circuit M=4/8 **100%**; **M₀ = 4** required, M=8 informational until more runs. Hop RSS ~5–8 MiB across the sweep. |
| **E** | After first hop-call green | Initially **stop**; continued: B-CONFLICT + B-MSG+CALL scaffolds landed (thin client + loopback gtests). Product `CallLibp2pMediaBridge` glue and GUI remain deferred. |

---

## Explicit non-goals (until later)

- Replacing in-process gtests with Docker
- PR-blocking large-N capacity
- Full GUI as primary stress vehicle
- One merged suite with shared pass criteria for node and browser

---

## Delivery sequence (reference)

1. **Phase 0** — this doc + IMAGE_SMOKE cross-link.
2. **Phase 1** — cheapest Tier B gaps (core compose listed above; media-key Put/Load; K-cycle teardown).
3. **Phase 2** — IMAGE_SMOKE L2 = **`N-FANOUT`** (`pp-node-probe --mode media-fanout`).
4. **Phase 3** — **`N-CAP-MEDIA` sweep** (`--suite cap`); N-CAP-CIRCUIT; N-SOAK / N-CHAOS scaffolds (`--suite soak` / `--suite chaos`).
5. **Phase 4** — thin-client **`B-CALL-DIRECT`** then **`B-CALL-HOP`** (`--suite call-hop`). Then **B-CONFLICT** / **B-MSG+CALL** scaffolds (`--suite conflict` / `--suite msg-call`). Then interference mix (`--suite mix`).
