# Peer-scoped broadcast — decisions

**Status:** Active (project ADRs). Promote wire outcomes to `docs/contracts/` when shipped.  
**Design:** [DESIGN.md](DESIGN.md) · **Media scale:** [MEDIA_TREE.md](MEDIA_TREE.md) · **Program:** [PROGRAM.md](PROGRAM.md)  
**ADRs:** B001–B008

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

## B005 — Viewers settle at capacity leaves; relays are `help_media` Nodes

**Date:** 2026-09-05  
**Updated:** 2026-09-05 — discovery is recursive ladder ([B007](#b007--recursive-whitelist-ladder-discovery-admit-or-redirect)); not a central leaf directory.  
**Decision:** Audience **settles** on a hop with free viewer capacity (typically a leaf), not preferentially on the seed/root. Tree relays are durable Nodes with explicit **`help_media`** for that publisher/program, scoped per [RELAY_SCOPE.md](../p2p-mesh/RELAY_SCOPE.md). PreferLocal publisher Node may be root. How viewers **find** that hop is [B007](#b007--recursive-whitelist-ladder-discovery-admit-or-redirect).  
**Rationale:** Moves egress off seed/first tier; reuses helper whitelist mental model from [DESIGN.md](DESIGN.md#shared-helper-whitelist-product).  
**Alternatives:** All viewers on org seed (rejected for scale); open unauthenticated relay market in v1 (rejected).

---

## B006 — Spine F sequencing after C (tree after single-hop live)

**Date:** 2026-09-05  
**Decision:** Program order stays A→B→C→D→E for announce helpers and CAS; **Spine F (media tree)** starts after Spine C single-hop live is dogfoodable. B0 (ticket + stable key on one hop) may complete inside C; **B1 (2-tier media + ladder discovery)** is the first Spine F exit. Do not block D (`help_announce`) on F, but **`help_media` tree roles** feed F.  
**Rationale:** Prove tip→watch on one hop before investing in relay-of-relay, redirect, and slot-win demotion.  
**Alternatives:** Tree before tip+live (rejected — no product vertical); tree only as ops multi-seed (insufficient alone).

---

## B007 — Recursive whitelist ladder discovery (admit-or-redirect)

**Date:** 2026-09-05  
**Decision:** Multi-level audience placement uses a **recursive ladder**, not a coordinator leaf map and not SoftMigrate.

1. **Tip / heartbeat** lists only the publisher’s **immediate** `help_media` whitelist ∩ **online** PeerIds (L1 hints), plus `join_handle` / program ids. Tips do **not** carry the full tree.
2. **Publisher mints** the join ticket (stable media key grant, viewer-bound). Ticket is valid at any hop for that program/media epoch; publisher is **not** responsible for placing every viewer on a leaf.
3. **Every hop** (publisher-facing L1 and deeper relays) runs the **same** policy toward a viewer presenting a valid ticket:
   - free **viewer slot** → admit and attach as subscriber;
   - else → **redirect** to one or more of *that hop’s* whitelist ∩ online children (capacity-weighted + jitter + [RELAY_SCOPE](../p2p-mesh/RELAY_SCOPE.md) affinity);
   - redirect carries a budget / path stamp to prevent loops.
4. **Root overflow is discouraged** — fullness at L1 should deepen the tree, not pin N viewers on the publisher hop.
5. **Slot win + ladder demotion (relay join):** when a new whitelist relay attaches as a **relay-child** and the parent is at degree/slot pressure, the parent **may prefer the relay** over existing **viewer** (or lower-priority) occupants. Demoted parties receive a **redirect one rung down** (ideally onto the new relay) with a short grace window — not a hard kick off the program. Rate-limit slot wins so flapping relays cannot churn the audience.
6. **Whitelist model (v1):** each hop maintains its own `help_media` allowlist for children it will parent; optional publisher-signed “may relay program P until T” grant may be required in addition (hybrid). Open stranger relay market stays out of v1 ([B005](#b005--viewers-settle-at-capacity-leaves-relays-are-help_media-nodes)).

**Rationale:** Matches contact/`help_media` mental model; keeps announce tips small; scales placement without a central leaf directory; demotion turns new relay capacity into immediate seed/first-tier relief.  
**Alternatives:** Coordinator assigns primary+alternate leaves on every ticket (rejected as default — control-plane hotspot, weak PreferLocal); pure viewer-chosen open leaf ads (rejected in v1 — herd/stale/abuse); SoftMigrate mass roster (rejected — [B001](#b001--broadcast-is-not-a-large-group-call)).  
**Spec detail:** [MEDIA_TREE.md § Recursive ladder discovery](MEDIA_TREE.md#recursive-ladder-discovery-b007).

---

## B008 — Broadcast client code leaves the call stack

**Date:** 2026-09-26
**Decision:** The broadcaster and viewer become a sibling feature (`feature/broadcast`) sharing only feature-neutral layers with calls (reach, `media_relay` attach, capture / playback pipelines, device leases). The viewer path stops riding the group-joiner topology path; `BroadcastSessionCoordinator` and the call-side broadcast hooks are removed once the viewer workflow lands.
**Rationale / plan:** [media-client-layers L001–L007](../media-client-layers/DECISIONS.md); phases l1–l6 in [media-client-layers PHASES](../media-client-layers/PHASES.md).

## B009 — Video levels: opaque ordered integers, negotiated per relay at attach

**Date:** 2026-09-30
**Decision:**
- **Levels.** Video carries a quality level: an opaque ordered integer (1–15; higher = more bits). Only the order is shared; what a level means (size, fps, bitrate) is the client's. In practice at most two are used. Audio has no level. The track kind and level ride in the media `channel_id` ([MEDIA_CHANNELS.md](../../docs/contracts/MEDIA_CHANNELS.md)).
- **One target, negotiated levels.** A publisher publishes to one relay the user chose. At attach it **offers** the levels it can produce (a phone: one; a desktop: more) and how many it can send at once; the relay **answers** with the levels it will carry, from its operator policy (levels it serves, how many it carries, strict or not). The publisher encodes exactly the answer.
- **Fallback over refusal.** When none of the offered levels is one the relay serves, the relay carries the offered level closest to what it serves (a high-level relay still carries a phone's low level) — unless its operator marked it strict, which refuses.
- **The relay enforces its answer.** A relay drops, at ingest, video frames of levels it did not agree to carry for that participant: an operator sizes relays by the levels they carry.
- **The tip names what is published.** The Live tip lists the levels actually published (the answer), not the relay's capability: a viewer only ever sees levels that exist. The viewer prefers a level and takes the nearest published one (below first).
- **No compatibility shims.** Not yet released: the quote carries the offer / answer, and today's single video channel becomes video level 1.

**Rationale:** Relays that carry only some levels stay simple (no per-viewer layer switching) and let operators provision high-level relays separately; the one-target publisher and the automatic answer keep the user's choice to "which relay", never "which levels". Phases: [PHASES § V — Video levels](PHASES.md#v--video-levels-b009).

