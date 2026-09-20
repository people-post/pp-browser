# Hard lab — current state

**Last updated:** 2026-09-20

## Direction

Forced-hop / discovery / impairment lab. **Wave 1–3 (partial) scaffold complete**. Wave 5 CGNAT + HL004 dirty-book / product-stack control plane landed. **Success-first** routing mode map in [HARD_LAB.md](../../packaging/pp-node/HARD_LAB.md#routing-mode-coverage-success-oracles).

## Landed

| Area | State |
|------|-------|
| HL004 E2E closeness | Accepted — CallStack+Amp on netns (not GUI); dirty-book first-class |
| `--suite hard-w5` default `--phase all` | circuit + product + **dirty** + **stack** |
| B-HARD-CALL-NAT-DIRTY | Green: `--reach bridge --force-dial-fail` + hop MarkHot (2s KA) + ClearDialBackoff |
| B-HARD-CALL-NAT-STACK | `pp-call-probe --product-stack`: **CallStack+CallUiBackend** StartCall/Accept/Leave over Amp chat; MeshHost `AttachAmpStack` + `OnMeshServicesStarted` (real AmpCircuitHopReach; no BindTestMediaPath) |
| CallMediaPlane test seam | `BindTestMediaPath` remains **gtest-only**; hard-lab stack uses production Wire |
| Bridge Ensure account→PeerId | `EnsurePeerReachableAsync` resolves `account:` via `MeshPeerIdForAccount` before circuit/punch (gtest `EnsureReachResolvesAccountToMeshPeerId`) |
| Dial-backoff gtest | `amp_circuit_hop_reach_test` + `call_media_bridge_answerer_start_test` |
| Ensure→circuit heal | Bridge ClearDialBackoff on EnsureAssociation miss (Abort only while dial still in flight — finished callback must not double-drop) |
| ConnectFailed stops media | gtest `CircuitHopMissStopsMediaOnConnectFailed` — not a hard-lab fail gate |
| AmpCircuitHopReach taxonomy | per-relay info logs + last-fail error string |
| PeerLinkManager::ClearDialBackoff / AbortInflightDial | Abort no longer DropLink mid-handshake; FinishDial suppress-backoff; cold-evict uses DropLink |
| CircuitTunnelCoordinator | `ScheduleWhenChannelOpen` looks up PeerLink by key (no raw pointer across Io posts) |

## Gaps

| Area | State |
|------|-------|
| Phase-2 `--reach product` probe helper | Still probe-local `EnsureProductCallMediaReach` (lower-tier oracle); stack mode owns product Ensure |
| N-HARD-DIR / DHT / N-ADMIT-HARD | Blocked on product hooks |
| Wave 4+ multi-hop | Blocked on L3.5 |
| GUI / phones | Manual dogfood only |
| SFU `media_relay` on hard topo | N≥3 harness — Wave 1 B-HARD-CALL when ready |

## Next

1. Keep hard-w5 `all` green in nightly (success oracles); stack fail → Bridge/reach fix + gtest promote
2. Optionally retire Phase-2 probe EnsureProduct when stack path covers punch∥circuit on dual-SNAT
3. N-HARD-DIR / DHT when hooks exist
