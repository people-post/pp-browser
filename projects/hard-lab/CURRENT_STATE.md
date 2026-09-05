# Hard lab — current state

**Last updated:** 2026-09-03

## Direction

Forced-hop / discovery / impairment lab. **Wave 1 complete** (N-HARD-FORCE + B-HARD-CALL + B-HARD-MSG+CALL). Next: Wave 2 impairments.

## Landed

| Area | State |
|------|-------|
| Project docs | README, DESIGN, DECISIONS, PHASES, CURRENT_STATE |
| Ladder / topology / profiles | [`packaging/pp-node/HARD_LAB.md`](../../packaging/pp-node/HARD_LAB.md) |
| Ops purpose rows + Gate F | [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md) |
| `docker-compose.hard-lab.yml` | Dual-homed hop + peer-a/net_a + peer-b/net_b; status **18618** |
| `Dockerfile.hard-peer` | Debian trixie sidecar for host-built probes |
| `pp_hard_lab_lib.sh` | Shared compose/IP/hop-MA helpers |
| `pp-node-probe` hard modes | `bridge-target`, `bridge-via-hop`, `media-recv`, `media-send` |
| `pp_hard_force_smoke.sh` | Isolation + circuit + media |
| `pp_hard_call_smoke.sh` | B-HARD-CALL / `--with-chat` → B-HARD-MSG+CALL |
| `--suite hard` | force → call → msg-call |
| Nested-chat reachability | `AmpDirectChatService::IsPeerReachable` accepts `IsConnected` (circuit nested); gtest `AmpDirectChatCircuitNestedTest` |

## Gaps

| Area | State |
|------|-------|
| Wave 2+ impairments / discovery | Not started |

## Next

1. **h2** — netem profiles (`N-HARD-LOSSY` / `ASYM` / `BW`)  
2. Multi-hop hard-lab blocked on media-hop L3.5  
