# Hard lab — current state

**Last updated:** 2026-09-10

## Direction

Forced-hop / discovery / impairment lab. **Wave 1 + Wave 2 scaffold complete**. Next: Wave 3 discovery.

## Landed

| Area | State |
|------|-------|
| Project docs | README, DESIGN, DECISIONS, PHASES, CURRENT_STATE |
| Ladder / topology / profiles | [`packaging/pp-node/HARD_LAB.md`](../../packaging/pp-node/HARD_LAB.md) |
| Ops purpose rows + Gate F | [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md) |
| `docker-compose.hard-lab.yml` | Dual-homed hop + peer-a/net_a + peer-b/net_b; peers `NET_ADMIN`; status **18618** |
| `Dockerfile.hard-peer` | Debian trixie sidecar for host-built probes (`iproute2`) |
| `pp_hard_lab_lib.sh` | Shared compose/IP/hop-MA + `pp_hard_link_apply` (clean/lossy/asym/bw) |
| `pp-node-probe` hard modes | `bridge-target`, `bridge-via-hop`, `media-recv`, `media-send` |
| `pp_hard_force_smoke.sh` | Isolation + circuit + media |
| `pp_hard_call_smoke.sh` | B-HARD-CALL / `--with-chat` → B-HARD-MSG+CALL |
| `pp_hard_link_smoke.sh` | N-HARD-LOSSY / ASYM / BW (+ one netem retry) |
| `--suite hard` | force → call → msg-call |
| `--suite hard-w2` | lossy → asym → bw |
| Nested-chat reachability | `AmpDirectChatTransport::IsPeerReachable` accepts `IsConnected` (circuit nested); gtest `AmpDirectChatCircuitNestedTest` |

## Gaps

| Area | State |
|------|-------|
| Wave 3+ discovery / multi-hop / NAT | Not started; multi-hop blocked on L3.5 |

## Next

1. **h3** — discovery (`N-HARD-STALE-ADDR` / `SEED-ONLY`; DIR/DHT when hooks exist)
2. Multi-hop hard-lab blocked on media-hop L3.5
