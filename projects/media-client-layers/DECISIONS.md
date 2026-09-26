# Media client layers — decisions

Prefix **L**. Status lives in [CURRENT_STATE.md](CURRENT_STATE.md); spec in [DESIGN.md](DESIGN.md).

---

## L001 — Broadcast is a sibling feature of calls, not part of the call path

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** Broadcast (broadcaster + viewer) lives in its own `feature/broadcast` with its own entry, state and lifecycle — no invite / ring / accept, no `CallLifecycle`, no `CallSessionStore` records. It shares only feature-neutral lower layers with calls. Viewers are receive-only; the broadcaster is send-only.
**Rationale:** The mechanisms differ (admission via signed tickets and admit-or-redirect, one-way media, relay tree) — extends [B001](../peer-scoped-broadcast/DECISIONS.md#b001--broadcast-is-not-a-large-group-call) from session shape to code structure. No UI is wired yet, so separating now is cheap.
**Alternatives:** Keep the viewer on the group-joiner path (rejected — attaches as a publisher, shares SoftMigrate state, skips ticket / redirect); a base class for 1:1 / group / broadcast (rejected — policies differ; share mechanism only).

---

## L002 — Shared layers must be feature-neutral

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** Reach (`PeerReachCoordinator`), relay session attach (`MediaRelayAttachCoordinator`), capture / playback pipelines and device leases live below both features (`domain/mesh`, `domain/media`). Test: outside both features, and the API uses none of *call*, *roster*, *program*, *ticket*; credentials and session ids are opaque.
**Rationale:** Same split that made 1:1 clean; lets calls and broadcast evolve independently; respects the layer rule (no feature → feature edges for shared seams).

---

## L003 — Device leases per kind, exclusive policy for now

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** A `DeviceArbiter` grants leases per device kind (mic / camera / speaker). Policy today: exclusive, refusing with a clear reason. Pipelines are instances owned by the lease holder — no singleton "one engine serves one call".
**Rationale:** Running a call and a broadcast at once is not needed now but must not be blocked; with leases it becomes a policy change (plus playback mixing, which already mixes remote streams), not a redesign.

---

## L004 — Stream plans carry channels generically

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** Stream plans and pipelines carry channels generically (as `SfuPacket::channel_id`: 0 = Opus, 1 = H264). The first viewer cut subscribes to channel 0 only; the broadcaster's capture keeps camera as an optional input.
**Rationale:** Viewer video is needed later but not on day one; it must be additive (new channel + decode path), not an API change.

---

## L005 — A tree relay pulls upstream with the viewer client

**Date:** 2026-09-26
**Status:** Proposed (lands with peer-scoped-broadcast B1)
**Decision:** A child relay's upstream leg uses the same viewer client (credential → admit-or-redirect → subscribe) as an end viewer. The broadcast relay role = viewer of its parent + server for its children.
**Rationale:** One client flow builds the tree; no separate relay-to-relay protocol; the viewer code gets double coverage. Consistent with [B002](../peer-scoped-broadcast/DECISIONS.md#b002--broadcast-allows-multi-sfu-media-tree-calls-do-not) ("child media_relay subscribes upstream").

---

## L006 — Same media_relay data plane; own AEAD label per feature

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** Broadcast uses the same `media_relay` frames through blind relays. Its frame AEAD label is its own (e.g. `broadcast|program_id|…`), distinct from `call-media-sfu|call_id|…`, so frames cannot be confused across features. `MediaRelayTypes` `call_id` becomes a neutral session id.
**Rationale:** Reuses the proven relay path and [B003](../peer-scoped-broadcast/DECISIONS.md) encrypt-once; domain separation in the AEAD is cheap and prevents cross-use.
**Open:** whether the neutral rename touches wire field names (compat per [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md)) or stays internal.

---

## L007 — Stack on the l0 branch

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** l1 onward stack on `refactor/peer-reach-coordinator` (its PR is open) rather than waiting for merge.
**Rationale:** l1 moves the classes that branch created; stacking avoids a rebase of moved files.
