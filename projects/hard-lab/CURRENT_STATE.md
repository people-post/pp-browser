# Hard lab — current state

**Last updated:** 2026-09-20

## Direction

Forced-hop / discovery / impairment lab. **Wave 1–3 (partial) scaffold complete**. Wave 5 CGNAT + HL004 dirty-book / product-stack control plane landed.

## Landed

| Area | State |
|------|-------|
| HL004 E2E closeness | Accepted — CallStack+Amp on netns (not GUI); dirty-book first-class |
| `--suite hard-w5` default `--phase all` | circuit + product + **dirty** + **stack** |
| B-HARD-CALL-NAT-DIRTY | Green: `--reach bridge --force-dial-fail` + hop MarkHot (2s KA) + ClearDialBackoff |
| B-HARD-CALL-NAT-STACK | `pp-call-probe --product-stack` (Invite/Accept + CallLifecycle + dirty media) |
| CallMediaPlane test seam | `BindTestMediaPath(transport, dial, circuit_reach)` for full CallStack wire next |
| Dial-backoff gtest | `amp_circuit_hop_reach_test` + `call_media_bridge_answerer_start_test` |
| PeerLinkManager::ClearDialBackoff / AbortInflightDial | Abort no longer DropLink mid-handshake; FinishDial suppress-backoff; cold-evict uses DropLink |
| CircuitTunnelCoordinator | `ScheduleWhenChannelOpen` looks up PeerLink by key (no raw pointer across Io posts) |

## Gaps

| Area | State |
|------|-------|
| Full CallStack CSM Invite wire on hard topo | Seam ready; probe still uses CallLifecycle + chat envelopes |
| N-HARD-DIR / DHT / N-ADMIT-HARD | Blocked on product hooks |
| Wave 4+ multi-hop | Blocked on L3.5 |
| GUI / phones | Manual dogfood only |

## Next

1. Promote product-stack to CallStack+CallUiBackend StartCall/Accept on Amp delivery (use BindTestMediaPath+circuit)
2. Keep hard-w5 `all` green in nightly
3. N-HARD-DIR / DHT when hooks exist
