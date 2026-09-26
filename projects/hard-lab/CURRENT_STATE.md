# Hard lab — current state

**Last updated:** 2026-09-26

## Direction

Forced-hop / discovery / impairment lab. **Wave 1–3 (partial) scaffold complete**. Wave 5 CGNAT + HL004 dirty-book / product-stack control plane landed. **Success-first** routing mode map in [HARD_LAB.md](../../packaging/pp-node/HARD_LAB.md#routing-mode-coverage-success-oracles).

## Landed

| Area | State |
|------|-------|
| HL004 E2E closeness | Accepted — CallStack+Amp on netns (not GUI); dirty-book first-class |
| `--suite hard-w5` default `--phase all` | circuit + **stack** + **cold** + **cold-dirty** + **cold-await** (product / dirty retired) |
| B-HARD-CALL-NAT-COLD / -DIRTY / -AWAIT | Product stack with `--signal-dir` (call control via `/share` files, no Amp-chat pre-path) — media reach starts cold. Gates: offerer `PeerReachCoordinator` cold `mode=reach`; dirty adds H010 skip-private; await (offerer uplink delay) adds answerer cold `mode=await`; ≥ 100 rx frames on both sides. Closes the gap where Phase-4 only hit the reuse shortcut (its Amp-chat signaling pre-builds the circuit) |
| B-HARD-CALL-NAT-DIRTY | **Retired** — superseded by COLD-DIRTY (product reach + forced dial miss, no probe re-warm); see HL004 update |
| B-HARD-CALL-NAT-STACK | `pp-call-probe --product-stack`: **CallStack+CallUiBackend** StartCall/Accept/Leave over Amp chat; MeshHost `AttachAmpStack` + `OnMeshServicesStarted` (real AmpCircuitHopReach; no BindTestMediaPath) |
| CallMediaPlane test seam | `BindTestMediaPath` remains **gtest-only**; hard-lab stack uses production Wire |
| Bridge Ensure account→PeerId | Bridge resolves `account:` via `MeshPeerIdForAccount` before `PeerReachCoordinator` circuit/punch (gtest `EnsureReachResolvesAccountToMeshPeerId`) |
| Dial-backoff gtest | `amp_circuit_hop_reach_test` + `call_media_bridge_answerer_start_test` |
| Ensure→circuit heal | Bridge ClearDialBackoff on EnsureAssociation miss (Abort only while dial still in flight — finished callback must not double-drop) |
| ConnectFailed stops media | gtest `CircuitHopMissStopsMediaOnConnectFailed` — not a hard-lab fail gate |
| AmpCircuitHopReach taxonomy | per-relay info logs + last-fail error string; tunnel miss logs `preferred_ma=` |
| Circuit hop dial-book gate | `CircuitHopDialBookAllowsRegister` — CollectDialable / WarmBootstrap skip private hop MAs (dogfood sendto poison); gtest `PrivateHopMaDoesNotPoisonPublicPreferred` |
| PeerLinkManager::ClearDialBackoff / AbortInflightDial | Abort no longer DropLink mid-handshake; FinishDial suppress-backoff; cold-evict uses DropLink |
| CircuitTunnelCoordinator | `ScheduleWhenChannelOpen` looks up PeerLink by key (no raw pointer across Io posts) |

## Gaps

| Area | State |
|------|-------|
| Nested-circuit dial timeout after a forced dial miss | Seen once in COLD-DIRTY (`EnsureViaCircuit nested miss … dial timeout`, 2026-09-26); not reproduced in 25 runs since. Suspect: the aborted dial's link entry under the target PeerId (Amp k1 "LinkTable::Insert on an occupied dial key"). Watch nightly |
| N-HARD-DIR / DHT / N-ADMIT-HARD | Blocked on product hooks |
| Wave 4+ multi-hop | Blocked on L3.5 |
| GUI / phones | Manual dogfood only |
| SFU `media_relay` on hard topo | N≥3 harness — Wave 1 B-HARD-CALL when ready |

## Next

1. Keep hard-w5 `all` green in nightly (success oracles); stack fail → Bridge/reach fix + gtest promote
2. ~~Retire Phase-2 / Phase-3 probe reach copies~~ — done (COLD phases)
3. N-HARD-DIR / DHT when hooks exist
