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
**Resolved (l2):** the wire field stays `"call_id"` — relays (pp-node) parse it; changing it would break mixed versions ([COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md)). Only the client API is neutral: `IMediaRelayClient` parameters and `MediaRelayQuoteRequest::session_id`.

---

## L007 — Stack on the l0 branch

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** l1 onward stack on `refactor/peer-reach-coordinator` (its PR is open) rather than waiting for merge.
**Rationale:** l1 moves the classes that branch created; stacking avoids a rebase of moved files.

---

## L008 — Two kinds of reach: link reach and service reach

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** Keep two reach operations in `domain/mesh/reachability`, not one:
- **Link reach** — `PeerReachCoordinator`: success = a Connected PeerLink to the peer (dial → seed park → circuit as a *nested session* → punch; Reach / Await). Used by 1:1 call media.
- **Service reach** — `ICircuitHopReach::TryEnsureHopReachable` (`AmpCircuitHopReach`): success = the node's `media_relay` service is dialable — an endpoint, or a *protocol-keyed* circuit hop registered for `kMediaRelayProtocolId` (`register_endpoint`, not nested); punch first, then circuit. Used by relay attach (group joiner today, broadcast viewer / broadcaster later).
**Rationale:** Found while doing l1: routing hops through `PeerReachCoordinator` would move `media_relay` from protocol-keyed circuit hops onto nested links — a transport change, not a refactor. Both are feature-neutral and now live side by side; l2's `MediaRelayAttachCoordinator` uses service reach.
**Revisit:** if `media_relay` ever runs over nested links (e.g. with the k3 path set), link reach can subsume service reach.

---

## L009 — Relay attach is a stateless capability; recovery stays with each feature

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** The shared relay-attach layer is `AttachToMediaRelayAsync` (`domain/mesh/l4/media_relay/MediaRelayAttach.*`): ports (relay client, dial registry, service reach) + request (hop, dial hint, opaque session id / auth, caller-built quote request) + hooks (`accept_quote`, `still_wanted`, `on_frame`) → `MediaRelayAttached{quote_id, a_up_bps}`. It holds no state. Frame decryption, quote sizing, pricing, what happens after attach, and loss recovery (group: reattach with backoff; viewer: re-admit / redirect) stay in the feature.
**Rationale:** Reading the group path showed the mechanism was copy-pasted twice (attach and guest reattach) and carried no state of its own; recovery differs per feature, so a stateful "coordinator" owning it would re-couple the features. The planned name `MediaRelayAttachCoordinator` is dropped (free-function capability naming, AGENTS role table).


---

## L010 — l3 splits: spec first, then a device owner thread

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** l3 lands as l3a (session spec on the existing `CallMediaEngine`), l3b (`DeviceArbiter` with one **device thread** owning SDL open / close / reopen-on-loss; pipelines receive streams through leases) and l3c (pipeline instances / rename, only if l4 / l5 need it). The local video encoder is created with the camera, not the session.
**Rationale:** The engine's capture thread both owns the devices (opens both, reopens on loss, under the engine mutex) and paces sends; playout mixes into the stream that thread opened. A spec is a small change on that model; leases are not — revoking or handing over a device from another thread would race the capture thread's reopen. Giving devices one owner thread is the fix that makes leases sound, and it is cheaper before a second consumer (viewer / broadcaster) exists than after. The user prefers the better fix over the safer one and accepts threading risk earlier (2026-09-26).
**Consequence:** l3b is a threading change inside `domain/media`; calls keep behavior and are verified with the engine gtests, TSan media + call suites and hard lab.

---

## L011 — Speaker is shared, mic exclusive; one device thread either way

**Date:** 2026-09-26
**Status:** Accepted (refines L003's "exclusive policy for now")
**Decision:** `MediaDeviceArbiter` policy per kind: **mic exclusive** (a second holder is refused with the holder's name and runs without a mic), **speaker shared** (the OS mixes playback streams). Every open / close / reopen, of any kind, runs on the arbiter's single device thread.
**Rationale:** The ringtone plays over an active call when a second invite rings (`ring_.active` during `in_call_`); an exclusive speaker would silently drop that. The speaker conflict that existed was never sharing — it was an open racing another holder's close on different threads (ringtone `DestroyAudioStream` vs call-media `OpenAudioDeviceStream`, the Samsung Accept hang), patched with `WaitUntilPlaybackDeviceReleased`. Serializing device operations fixes that structurally, so sharing is safe; two captures of one mic, on the other hand, would double-send the user's voice.
**Consequence:** Viewer + call can both play (not blocked, L003 goal); a second duplex session gets no mic until the first releases it. Policy is a `MediaDeviceSharePolicy` value — changeable without touching pipelines.
