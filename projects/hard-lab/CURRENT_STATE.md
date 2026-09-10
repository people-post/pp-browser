# Hard lab — current state

**Last updated:** 2026-09-10

## Direction

Forced-hop / discovery / impairment lab. **Wave 1–3 (partial) scaffold complete** — STALE-ADDR + SEED-ONLY landed; DIR/DHT/ADMIT-HARD still gated.

## Landed

| Area | State |
|------|-------|
| Project docs | README, DESIGN, DECISIONS, PHASES, CURRENT_STATE |
| Ladder / topology / profiles | [`packaging/pp-node/HARD_LAB.md`](../../packaging/pp-node/HARD_LAB.md) |
| Ops purpose rows + Gate F | [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md) |
| `docker-compose.hard-lab.yml` | Dual-homed hop + peer-a/net_a + peer-b/net_b; peers `NET_ADMIN`; status **18618** |
| `Dockerfile.hard-peer` | Debian trixie sidecar for host-built probes (`iproute2`) |
| `pp_hard_lab_lib.sh` | Shared compose/IP/hop-MA + `pp_hard_link_apply` (clean/lossy/asym/bw) |
| `pp-node-probe` hard modes | `bridge-target` (`--warm-hop`), `bridge-via-hop` (`--peer-id-only`), `direct-expect-fail`, `media-recv`, `media-send` |
| `pp_hard_force_smoke.sh` | Isolation + circuit + media |
| `pp_hard_call_smoke.sh` | B-HARD-CALL / `--with-chat` → B-HARD-MSG+CALL |
| `pp_hard_link_smoke.sh` | N-HARD-LOSSY / ASYM / BW (+ one netem retry) |
| `pp_hard_disco_smoke.sh` | N-HARD-STALE-ADDR / SEED-ONLY |
| `--suite hard` | force → call → msg-call |
| `--suite hard-w2` | lossy → asym → bw |
| `--suite hard-w3` | stale-addr → seed-only |
| Nested-chat reachability | `AmpDirectChatTransport::IsPeerReachable` accepts `IsConnected` (circuit nested); gtest `AmpDirectChatCircuitNestedTest` |

## Gaps

| Area | State |
|------|-------|
| N-HARD-DIR / DHT / N-ADMIT-HARD | Blocked on directory lab hooks, DHT peers in hard nets, pp-node deploy-profile admission |
| Wave 4+ multi-hop / NAT | Multi-hop blocked on L3.5 |

## Next

1. **N-HARD-DIR** / **DHT** / **N-ADMIT-HARD** when product hooks exist
2. Multi-hop hard-lab blocked on media-hop L3.5
