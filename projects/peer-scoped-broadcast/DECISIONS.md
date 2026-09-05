# Peer-scoped broadcast — decisions

**Status:** Active (project ADRs). Promote wire outcomes to `docs/contracts/` when shipped.  
**Design:** [DESIGN.md](DESIGN.md) · **Media scale:** [MEDIA_TREE.md](MEDIA_TREE.md) · **Program:** [PROGRAM.md](PROGRAM.md)

---

## B001 — Broadcast is not a large group call

**Date:** 2026-09-05  
**Decision:** Live audience scale is a **separate session shape** from group calls. Do **not** raise call soft-max ([V007](../p2p-av-calls/DECISIONS.md#v007--participant-cap-16-soft-engineering-floor-8)) or SoftMigrate into a mass-audience fan-out. Broadcast uses tip → join ticket → realtime attach; pickup is Notifications/banner, not ringing.  
**Rationale:** Call roster, rotate-on-leave, and one-hop SFU assumptions break at large N; stretching them couples UX and load incorrectly.  
**Alternatives:** SoftMigrate to “big SFU” only (rejected — seed egress still ~N); full-mesh (rejected).

---

## B002 — Broadcast allows multi-SFU media tree; calls do not

**Date:** 2026-09-05  
**Decision:** **Broadcast** may copy sealed realtime frames through a **degree-capped tree** of blind `media_relay` nodes ([MEDIA_TREE.md](MEDIA_TREE.md)). **Group calls** keep the existing non-goal: one subcontracted media hop; circuit multi-hop is reachability only ([MULTI_HOP_CIRCUIT.md](../media-hop-reachability/MULTI_HOP_CIRCUIT.md)). Hard-lab `N-HARD-MULTI-HOP-MEDIA` stays call-off unless product revisits calls.  
**Rationale:** Seed/first-tier pressure under massive subscribers is a media-capacity problem; circuit brokers do not multiply bytes.  
**Alternatives:** More org seeds only (ops help, no log scaling); CDN off-mesh for all live (deferred / optional overflow later).

---

## B003 — Keep encrypt-once AEAD for broadcast

**Date:** 2026-09-05  
**Updated:** 2026-09-05 — all hops **must** carry opaque blobs; cleartext escape removed.  
**Decision:** Keep app-layer **encrypt-once AEAD** under a **session broadcast key**; hops remain blind and copy ciphertext (same seal family as [V004](../p2p-av-calls/DECISIONS.md#v004--shared-call-media-key-not-group-n-ciphertext)). Encryption is **mandatory**. Every `media_relay` hop (root, intermediate, leaf) **must** forward opaque sealed frames without inspecting or requiring plaintext. Do **not** introduce a cleartext media path.  
**Rationale:** AEAD cost is negligible vs egress; blindness enables PreferLocal / contact / `help_media` relays; cleartext does not reduce seed N× bitrate. Opaque-blob transport is a hop capability requirement, not an optional fallback.  
**Alternatives:** Cleartext media (rejected); per-viewer encrypt (rejected — kills tree copy).

---

## B004 — Stable session key + join ticket (not rotate-on-viewer-leave)

**Date:** 2026-09-05  
**Decision:** Broadcast uses a **stable session key** (or long epoch) for the show. Key distribution via **publisher-signed join ticket** (wrap or key id + grant). **Do not** rotate on every viewer leave. Rotate on end, revoke, or kick/ban epoch; tip may carry epoch bump (heartbeat floor bypass once).  
**Rationale:** Call leave-rotate ([V003](../p2p-av-calls/DECISIONS.md#v003--rotate-media-key-on-every-leave-overlapping-epochs)) does not scale to large audiences; ticket join matches tip discovery and late join.  
**Alternatives:** Pairwise wrap to every viewer (rejected at scale); MLS sender keys (deferred).

---

## B005 — Viewers attach to leaves; relays are `help_media` Nodes

**Date:** 2026-09-05  
**Decision:** Audience attaches to **leaf** (or nearest eligible) SFUs, not preferentially to the seed/root. Tree relays are durable Nodes with explicit **`help_media`** for that publisher/program, scoped per [RELAY_SCOPE.md](../p2p-mesh/RELAY_SCOPE.md). PreferLocal publisher Node may be root.  
**Rationale:** Moves egress off seed/first tier; reuses helper whitelist mental model from [DESIGN.md](DESIGN.md#shared-helper-whitelist-product).  
**Alternatives:** All viewers on org seed (rejected for scale); open unauthenticated relay market in v1 (rejected).

---

## B006 — Spine F sequencing after C (tree after single-hop live)

**Date:** 2026-09-05  
**Decision:** Program order stays A→B→C→D→E for announce helpers and CAS; **Spine F (media tree)** starts after Spine C single-hop live is dogfoodable. B0 (ticket + stable key on one hop) may complete inside C; **B1 (2-tier media)** is the first Spine F exit. Do not block D (`help_announce`) on F, but **`help_media` tree roles** feed F.  
**Rationale:** Prove tip→watch on one hop before investing in relay-of-relay and reparent.  
**Alternatives:** Tree before tip+live (rejected — no product vertical); tree only as ops multi-seed (insufficient alone).
