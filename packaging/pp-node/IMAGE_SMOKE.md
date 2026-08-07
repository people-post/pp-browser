# pp-node image / deploy smoke tests

**Tier:** ops dogfood  
**Related:** [CONFIGURATION.md](../../docs/ops/CONFIGURATION.md#pp-node-deploy-overlays), [BUILD.md](../../docs/ops/BUILD.md#headless-mesh-node-pp-node)

Automated checks for a **running** `pp-node` (Docker image, compose, or bare binary). These complement in-process gtests (`circuit_relay_service_test`, `media_relay_service_test`, …) which already own protocol correctness.

| Layer | What it proves | Tooling | Status |
|-------|----------------|---------|--------|
| **L0** | Process up; `/healthz` + `/status`; caps **started** (`circuit_relay` / `media_relay` flags) | `scripts/pp_node_image_smoke.sh` | **Done** |
| **L1** | From outside the hop: dial peer; **media** `RequestQuote`; **circuit** bridge + payload to a local target | `pp-node-probe` + `scripts/pp_node_relay_smoke.sh` | **Done** (scaffold) |
| **L2** | Multi-container topology (hop + 2 clients, fan-out / SoftMigrate-style) | Compose + probe or gtest-shaped harness | **Deferred** — see below |

## CI / release

Node images ship on the **`pp-node/v*`** train ([`release-pp-node.yml`](../../.github/workflows/release-pp-node.yml)), independent of app `v*` tags. Cut tags from **`main`**; day-to-day work is on **`develop`**. See [RELEASE.md](../../docs/ops/RELEASE.md).

Release CI runs **L0** against the pushed image. Run L0/L1 locally against compose as below. **L2** (multi-container fan-out) remains deferred.

## Prerequisites

```bash
# Local dogfood container (status published on host :18518)
docker compose -f packaging/pp-node/docker-compose.yml up -d --build

# L1 probe binary (desktop build tree)
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

Asserts: `ok`/`host_running`, non-empty `peer_id` + `listen`, expected boolean caps. Optional Bearer negative check when `PP_NODE_STATUS_TOKEN` is set.

Does **not** open libp2p streams.

## L1 — Relay probe

```bash
./scripts/pp_node_relay_smoke.sh          # L0 then L1
./scripts/pp_node_relay_smoke.sh --l0-only
```

`pp-node-probe` (under `src/app/node/probe/`):

1. Builds hop multiaddr from `/status` (`0.0.0.0` → `127.0.0.1`)
2. Starts ephemeral client + bridge-target hosts on loopback
3. **media_relay:** `RequestQuote` against the hop
4. **circuit_relay:** `RequestBridge` through the hop to the local target; writes a length-prefixed payload and expects delivery

Limitations (intentional for a thin L1):

- Media path stops at **quote** (no AcceptAndAttach / fan-out — covered by gtests + L2)
- Circuit bridge needs the hop to **dial back** to the probe’s target. Against Docker, set `PP_NODE_PROBE_ADVERTISE_HOST` (or rely on auto `docker0` / bridge gateway); `127.0.0.1` only works for a hop on the same host network namespace
- Admission policies that require contacts may reject strangers — org seed volunteer profile should allow probe

## L2 — Multi-node compose (resume later)

Goal: CI/dogfood topology mirroring `MediaRelayServiceTest` / `CircuitMediaRelayComposeTest` **across containers**.

Suggested shape (not implemented):

```text
services:
  hop:     # pp-node image, caps on, status + listen published
  client-a: # probe or thin client image; dials hop; AcceptAndAttach
  client-b: # second attach + Subscribe; assert fan-out frame
```

Work items when resuming:

1. Compose file under `packaging/pp-node/` (e.g. `docker-compose.relay-smoke.yml`) with fixed ports / shared network
2. Extend `pp-node-probe` with `--mode=media-fanout` (quote → attach ×2 → send frame) **or** ship a second binary reused from test helpers
3. Optional dial-back check: client listen + hop `DialBackService` probe via `/status` reachability fields
4. Gate in release/nightly CI only if runners have nested Docker + enough time (full libp2p build already heavy)

Do **not** reimplement full gtest matrices in shell; keep L2 as orchestration around the same integration APIs.

## Mapping to existing unit tests

| Concern | Prefer |
|---------|--------|
| Bridge framing, admission masks, fan-out QoS | In-tree gtests |
| Image ENTRYPOINT / env overlays / status flags | L0 |
| “Can a peer outside the container use this hop?” | L1 |
| Two phones + hop on a bridge network | L2 |

## Quick reference

| Env | Role |
|-----|------|
| `PP_NODE_STATUS_URL` | L0/L1 status base URL |
| `PP_NODE_STATUS_TOKEN` | Optional Bearer |
| `PP_NODE_EXPECT_CIRCUIT` / `PP_NODE_EXPECT_MEDIA` | L0 expected flags (`1`/`0`) |
| `PP_NODE_PROBE_HOP` | L1 hop multiaddr override |
| `PP_NODE_PROBE_ADVERTISE_HOST` | Host IP the Docker hop can dial for circuit target (often `docker0` gateway) |
| `PP_NODE_PROBE_BIN` | Path to `pp-node-probe` |
