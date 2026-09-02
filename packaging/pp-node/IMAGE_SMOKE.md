# pp-node image / deploy smoke tests

**Tier:** ops dogfood  
**Related:** [CONFIGURATION.md](../../docs/ops/CONFIGURATION.md#pp-node-deploy-overlays), [BUILD.md](../../docs/ops/BUILD.md#headless-mesh-node-pp-node), [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md) (purpose IDs `N-*` / tiers; L2 = **N-FANOUT**)

Automated checks for a **running** `pp-node` (Docker image, compose, or bare binary). These complement in-process gtests (`circuit_relay_service_test`, `media_relay_service_test`, …) which already own protocol correctness.

| Layer | What it proves | Tooling | Status |
|-------|----------------|---------|--------|
| **L0** | Process up; `/healthz` + `/status`; caps **started** (`circuit_relay` / `media_relay` flags) | `scripts/pp_node_image_smoke.sh` | **Done** |
| **L1** | From outside the hop: dial peer; **media** `RequestQuote`; **circuit** bridge + payload to a local target | `pp-node-probe` + `scripts/pp_node_relay_smoke.sh` | **Done** (scaffold) |
| **L2** | **N-FANOUT:** hop in container + two client hosts in probe; attach×2 + frame fan-out | `docker-compose.relay-smoke.yml` + `pp-node-probe --mode media-fanout` + `scripts/pp_node_fanout_smoke.sh` | **Done** (scaffold) |
| **N-DHT** | Two local `pp-node`s with `PP_NODE_CAP_DHT=1` discover each other’s ADP addrs (no Brief HTTP) | `scripts/pp_node_dht_smoke.sh` | **Done** (lab) |
| **N-CAP** | Soft media attach capacity curve (N=4 cheap; sweep via `--suite cap`) | `pp-node-probe --mode media-cap` + `scripts/pp_node_cap_smoke.sh` | **Done** (soft SLO N≤8; 12/16 informational) |
| **N-CAP-CIRCUIT** | Concurrent circuit bridges vs packaged hop | `pp-node-probe --mode circuit-cap` + `scripts/pp_node_circuit_cap_smoke.sh` | **Done** (soft SLO M≤4) |
| **N-SOAK / N-CHAOS** | Churn + restart/kill | `--suite soak` / `--suite chaos` | **Done** (not PR-blocking) |
| **N-MIX / B-MIX** | Parallel allowlisted smokes (interference) | `--suite mix` | **Done** (nightly; not in `all`) |

## CI / release

Node images ship on the **`pp-node/v*`** train ([`release-pp-node.yml`](../../.github/workflows/release-pp-node.yml)), independent of app `v*` tags. Cut tags from **`main`**; day-to-day work is on **`develop`**. See [RELEASE.md](../../docs/ops/RELEASE.md).

Release CI runs **L0** against the pushed image. Run L0/L1/L2 locally against compose as below. Gate **L2** in nightly/release only once stable (not every PR).

## Local driver (preferred)

[`scripts/pp_local_test.sh`](../../scripts/pp_local_test.sh) starts/stops the **relay-smoke** hop and runs L0–L2 / N-CAP (and optional unit/call/cap/soak/chaos/call-hop/mix suites). Individual `pp_*_smoke.sh` scripts remain for CI when the hop is already up.

```bash
# After packaging dist/pp-node/docker (see BUILD.md):
./scripts/pp_local_test.sh run --suite node     # up hop + L0/L1/fanout/cap N=4; hop stays up
./scripts/pp_local_test.sh run --suite cap      # media sweep 4,8,12,16 + circuit-cap
./scripts/pp_local_test.sh run --suite soak     # 120s churn (PP_NODE_SOAK_SEC=3600 weekly)
./scripts/pp_local_test.sh run --suite chaos    # kill-client / restart / pause
./scripts/pp_local_test.sh run --suite call-hop # B-CALL-HOP thin client
./scripts/pp_local_test.sh run --suite msg-call-hop # chat during hop call (same pair)
./scripts/pp_local_test.sh run --suite mix      # B-MIX + N-MIX + same-session hop chat (nightly)
./scripts/pp_local_test.sh status
./scripts/pp_local_test.sh stop                 # keep volume
./scripts/pp_local_test.sh clear                # down -v
```

Do **not** run [`docker-compose.yml`](docker-compose.yml) and [`docker-compose.relay-smoke.yml`](docker-compose.relay-smoke.yml) at once — both publish host **18517/18518**.

## Prerequisites (manual compose)

```bash
# Local dogfood container (status published on host :18518)
docker compose -f packaging/pp-node/docker-compose.yml up -d --build
# or L2-oriented hop (what pp_local_test.sh uses):
docker compose -f packaging/pp-node/docker-compose.relay-smoke.yml up -d --build

# Probe binary (desktop build tree)
cmake --build build --target pp-node-probe -j
```

Status HTTP must be reachable from the probe host (`PP_NODE_STATUS_ADDR=0.0.0.0:18518` + publish `18518`).

## L0 — HTTP smoke

```bash
./scripts/pp_node_image_smoke.sh
# or
PP_NODE_STATUS_URL=http://127.0.0.1:18518 ./scripts/pp_node_image_smoke.sh

# Caps intentionally off:
./scripts/pp_node_image_smoke.sh --expect-circuit=0 --expect-media=0
```

Asserts: `ok`/`host_running`, non-empty `peer_id` + Amp ADP `listen` (`/udp/…/adp/…`), expected boolean caps. Optional Bearer negative check when `PP_NODE_STATUS_TOKEN` is set.

Does **not** open Amp sessions or relay channels.

## L1 — Relay probe

```bash
./scripts/pp_node_relay_smoke.sh          # L0 then L1
./scripts/pp_node_relay_smoke.sh --l0-only
```

`pp-node-probe` (under `src/app/node/probe/`), default `--mode l1`:

1. Builds hop multiaddr from `/status` (`0.0.0.0` → `127.0.0.1`)
2. Starts ephemeral client + bridge-target hosts on loopback
3. **media_relay:** `RequestQuote` against the hop
4. **circuit_relay:** `RequestBridge` through the hop to the local target; writes a length-prefixed payload and expects delivery

Limitations (intentional for a thin L1):

- Media path stops at **quote** (no AcceptAndAttach / fan-out — covered by gtests + L2)
- Circuit bridge needs the hop to **dial back** to the probe’s target. Against Docker, set `PP_NODE_PROBE_ADVERTISE_HOST` (or rely on auto `docker0` / bridge gateway); `127.0.0.1` only works for a hop on the same host network namespace
- Admission policies that require contacts may reject strangers — org seed volunteer profile should allow probe

## L2 — N-FANOUT (hop process + two clients)

Purpose **N-FANOUT** in [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md): prove attach×2 and frame delivery against a **packaged hop** (not only in-process gtests).

Shape (implemented):

```text
hop:        pp-node container (docker-compose.relay-smoke.yml or docker-compose.yml)
client-a/b: two Amp stacks + MediaRelayService instances inside pp-node-probe --mode media-fanout
```

Clients share the probe process; the hop is a separate process/network namespace — enough to catch packaging/admission/env issues that loopback gtests miss. Full three-container clients remain optional later.

```bash
docker compose -f packaging/pp-node/docker-compose.relay-smoke.yml up -d --build
cmake --build build --target pp-node-probe -j
./scripts/pp_node_fanout_smoke.sh   # L0 then media-fanout
```

`pp-node-probe --mode media-fanout`:

1. Resolves hop multiaddr (script from `/status`, or `--hop`)
2. Starts two ephemeral client hosts
3. `RequestQuote` ×2 → `AcceptAndAttach` ×2 → `Subscribe` on B → `SendFrame` from A
4. Asserts B receives the frame payload

Do **not** reimplement full gtest matrices in shell; L2 orchestrates the same `MediaRelayService` APIs as `QuoteAcceptAttachFanout`.

## Mapping to existing unit tests

| Concern | Prefer |
|---------|--------|
| Bridge framing, admission masks, fan-out QoS | In-tree gtests |
| Image ENTRYPOINT / env overlays / status flags | L0 |
| “Can a peer outside the container use this hop?” | L1 |
| Packaged hop + attach×2 + frame (N-FANOUT) | L2 |
| Two phones + hop on a bridge network | Manual dogfood / later three-container |

## Quick reference

| Env | Role |
|-----|------|
| `PP_NODE_STATUS_URL` | L0/L1/L2 status base URL |
| `PP_NODE_STATUS_TOKEN` | Optional Bearer |
| `PP_NODE_EXPECT_CIRCUIT` / `PP_NODE_EXPECT_MEDIA` | L0 expected flags (`1`/`0`) |
| `PP_NODE_PROBE_HOP` | Hop multiaddr override |
| `PP_NODE_PROBE_MODE` | `l1` (default), `media-fanout`, `media-cap`, `circuit-cap`, `media-soak` |
| `PP_NODE_PROBE_ATTACHERS` | N or comma/sweep list for `media-cap` (default 4) |
| `PP_NODE_CAP_SWEEP` | `--suite cap` default `4,8,12,16` |
| `PP_NODE_PROBE_BRIDGES` | M or list for `circuit-cap` (default 4) |
| `PP_NODE_SOAK_SEC` | Soak duration seconds (default 120; weekly 3600) |
| `PP_NODE_PROBE_CHURN` | Attachers per soak round (default 4) |
| `PP_NODE_PROBE_ADVERTISE_HOST` | Host IP the Docker hop can dial for circuit target (often `docker0` gateway) |
| `PP_NODE_PROBE_BIN` | Path to `pp-node-probe` |
