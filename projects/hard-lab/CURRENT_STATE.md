# Hard lab — current state

**Last updated:** 2026-09-15

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
| `docker-compose.hard-lab-cgnat.yml` | Dual SNAT gw-a/gw-b + public hop; status **18628** |
| `Dockerfile.hard-gw` / `hard-gw-entrypoint.sh` | MASQUERADE + no unsolicited inbound |
| `pp_hard_nat_smoke.sh` | N-HARD-CGNAT-ISH + B-HARD-CALL-NAT (+PRODUCT); `--phase circuit\|product\|both` |
| `--suite hard-w5` | CGNAT-ish dual-NAT; default `--phase both`; `PP_HARD_NAT_CALL_EXPECT=fail` reproduce mode |
| `pp-call-probe` NAT flags | `--warm-hop`, `--peer-id-only`, `--min-rx-frames`, `--reach product` |
| Nested-chat reachability | `AmpDirectChatTransport::IsPeerReachable` accepts `IsConnected` (circuit nested); gtest `AmpDirectChatCircuitNestedTest` |
| AmpCircuitHopReach dual-NAT | Nested StartBridge is peer-id-only (no PreferredMultiaddr poison); call-media skips undialable has_endpoint EnsureAssociation |
| Lower-level NAT policy tests | `amp_circuit_hop_reach_test` + compose `PeerIdOnlyNestDoesNotPoisonRelayBookWithPrivateMa` / `PrivateTargetMultiaddrPoisonsRelayBook` (PR gate; hard-w5 remains topo wall) |

## Gaps

| Area | State |
|------|-------|
| N-HARD-DIR / DHT / N-ADMIT-HARD | Blocked on directory lab hooks, DHT peers in hard nets, pp-node deploy-profile admission |
| Wave 4+ multi-hop | Multi-hop blocked on L3.5 |
| Wave 5 remainder | Hairpin policy; UPnP/v6/path-migrate |

## Next

1. `--suite hard-w5` Phase-1 circuit + Phase-2 product punch→circuit are **green** on dual-SNAT (punch miss expected; circuit fall-through)
2. Use `PP_HARD_NAT_CALL_EXPECT=fail` only when locking a reproduce
3. **N-HARD-DIR** / **DHT** / **N-ADMIT-HARD** when product hooks exist
4. Multi-hop hard-lab blocked on media-hop L3.5
