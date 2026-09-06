# Peer-scoped broadcast — phases (media tree)

**Status:** Planning — checkboxes track **media scale** only. Announce spines stay in [PROGRAM.md](PROGRAM.md) / [CURRENT_STATE.md](CURRENT_STATE.md).  
**Spec:** [MEDIA_TREE.md](MEDIA_TREE.md) · **ADRs:** [DECISIONS.md](DECISIONS.md) B001–B007

Delivery order for live **media capacity** (Spine F). Do not start B1 until Spine C tip→watch on a **single** hop is dogfoodable.

---

## B0 — Broadcast session on one hop (Spine C completion)

Prerequisite for tree work; may land as C ribs.

- [x] Session flag / shape distinct from group SoftMigrate — `CallSessionKind::Broadcast` + SoftMigrate `is_broadcast` NoOp
- [x] SoftMigrate topology early-skip + `AcceptInvite` refuse for Broadcast sessions
- [x] Broadcast arm/accept extracted from call SoftMigrate stack — `BroadcastSessionCoordinator` (CSM / UI / facade delegate)
- [x] Stable session media key helpers (no rotate-on-viewer-leave) — `BroadcastJoinTicket` mint/apply
- [x] Join ticket (publisher-signed) delivers key / grant — domain mint/verify/apply + unit tests
- [x] Tip → ticket RPC codec (`ticket_request`/`ticket_response`) + arm apply
- [x] Amp ticket mint handler (`AmpBroadcastService::RequestTicket` / inbound)
- [x] Tip → ticket → attach to one `media_relay` (contact/seed / PreferLocal) — `ArmJoinFromLiveAnnounce` applies ticket into CallMediaKeyStore (attach still needs hop)
- [x] Encrypt-once AEAD mandatory; hops must copy opaque ciphertext (B003 locked)
- [ ] Loopback / lab: publisher + hop + ≥2 viewers

**Exit:** Watchable live from tip without treating audience as call roster.

---

## B1 — Two-tier tree + ladder discovery (first scale win) — Spine F exit

Locks [B007](DECISIONS.md#b007--recursive-whitelist-ladder-discovery-admit-or-redirect) at depth 2.

- [x] Tip / heartbeat lists publisher **L1 whitelist ∩ online** only (not full tree) — `l1_hop_peer_ids` on tip/codec/plan
- [ ] Root/L0 fans out to ≤`degree` child relays and/or viewers
- [ ] Child `media_relay` (`help_media`) subscribes upstream, fans out downstream (blind)
- [x] **Admit-or-redirect:** domain `BroadcastLadderLogic` + `BroadcastRpcCodec` viewer_attach(_result); `AmpBroadcastService` handler
- [x] **Slot win:** domain + `BroadcastRpcCodec` relay_slot_win(_result); `AmpBroadcastService` handler
- [ ] Degree + per-hop A↓ / ceiling enforced ([HOST_RECEIVE_POLICY](../p2p-av-calls/HOST_RECEIVE_POLICY.md))
- [ ] Lab: root + ≥1 child; L1 full → redirect; new relay join demotes a viewer downward; root egress ≉ N × bitrate

**Exit:** Demonstrable seed/first-tier pressure relief under N ≫ degree using ladder discovery (no central leaf map).

---

## B2 — Deeper ladder, election, repair

- [ ] Depth >2 under config cap (same admit-or-redirect at every hop)
- [ ] Richer child pick (free slots, depth, [RELAY_SCOPE](../p2p-mesh/RELAY_SCOPE.md) affinity)
- [ ] Capacity ads / refuse oversubscribed parents
- [ ] Reparent on parent death; `tree_epoch` / L1 tip refresh
- [ ] Redirect loop defenses hardened; slot-win rate limits / hysteresis
- [ ] Mid-show kick/ban → media epoch bump + ticket refresh path

**Exit:** Tree survives single relay loss and multi-rung redirects without full rebuild.

---

## B3 — Multi-root / overflow

- [ ] Regional / multi-root option (still blind hops + ladder)
- [ ] Paid / ops overflow seeds as additional L1s or deep capacity
- [ ] Hard-lab broadcast scenario: L1 hints, redirect chain, slot-win demotion (not call SoftMigrate overload)
- [ ] Promote wire / budgets to `docs/contracts/` as needed

**Exit:** Ops can add capacity without rewriting viewer join.

---

## Explicitly later / parking

- Simulcast / `video_hi` for adaptive tree layers
- Open helper marketplace (beyond whitelist)
- Cleartext media (rejected — [B003](DECISIONS.md#b003--keep-encrypt-once-aead-for-broadcast); hops must carry opaque blobs)
- Coordinator-assigned leaf on every ticket (optional ops mode; not default — [B007](DECISIONS.md#b007--recursive-whitelist-ladder-discovery-admit-or-redirect))
- Multi-SFU trees for **group calls** (remains non-goal)
