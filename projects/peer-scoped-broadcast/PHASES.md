# Peer-scoped broadcast — phases (media tree)

**Status:** Planning — checkboxes track **media scale** only. Announce spines stay in [PROGRAM.md](PROGRAM.md) / [CURRENT_STATE.md](CURRENT_STATE.md).  
**Spec:** [MEDIA_TREE.md](MEDIA_TREE.md) · **ADRs:** [DECISIONS.md](DECISIONS.md) B001–B006

Delivery order for live **media capacity** (Spine F). Do not start B1 until Spine C tip→watch on a **single** hop is dogfoodable.

---

## B0 — Broadcast session on one hop (Spine C completion)

Prerequisite for tree work; may land as C ribs.

- [ ] Session flag / shape distinct from group SoftMigrate (subscribe-only viewers)
- [ ] Stable session media key (no rotate-on-viewer-leave)
- [ ] Join ticket (publisher-signed) delivers key / grant
- [ ] Tip → ticket → attach to one `media_relay` (contact/seed / PreferLocal)
- [ ] Encrypt-once AEAD; hop copies ciphertext (keep V004 family)
- [ ] Loopback / lab: publisher + hop + ≥2 viewers

**Exit:** Watchable live from tip without treating audience as call roster.

---

## B1 — Two-tier media tree (first scale win) — Spine F exit

- [ ] Root SFU fans out to ≤`degree` child relays and/or viewers
- [ ] Child `media_relay` (`help_media`) subscribes upstream, fans out downstream (blind)
- [ ] Viewers **prefer leaf** attach; seed/root only as fallback
- [ ] Degree + per-hop A↓ / ceiling enforced ([HOST_RECEIVE_POLICY](../p2p-av-calls/HOST_RECEIVE_POLICY.md))
- [ ] Lab: root + ≥1 child hop + N viewers; root egress ≉ N × bitrate

**Exit:** Demonstrable seed/first-tier pressure relief under N ≫ degree.

---

## B2 — Depth, election, repair

- [ ] Depth >2 under config cap
- [ ] Relay parent selection (free slots, depth, [RELAY_SCOPE](../p2p-mesh/RELAY_SCOPE.md) affinity)
- [ ] Capacity ads / refuse oversubscribed parents
- [ ] Reparent on parent death; `tree_epoch` / root tip digest
- [ ] Mid-show kick/ban → epoch bump + ticket refresh path

**Exit:** Tree survives single relay loss without full rebuild.

---

## B3 — Multi-root / overflow

- [ ] Regional / multi-root option (still blind hops)
- [ ] Paid / ops overflow seeds as additional roots or deep capacity
- [ ] Hard-lab broadcast scenario (not call SoftMigrate overload)
- [ ] Promote wire / budgets to `docs/contracts/` as needed

**Exit:** Ops can add capacity without rewriting viewer join.

---

## Explicitly later / parking

- Simulcast / `video_hi` for adaptive tree layers
- Open helper marketplace (beyond whitelist)
- Cleartext media exception (only if hop cannot carry opaque blobs — [B003](DECISIONS.md#b003--keep-encrypt-once-aead-for-broadcast))
- Multi-SFU trees for **group calls** (remains non-goal)
