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

---

## L012 — Camera requests are asynchronous; the video thread owns camera and encoder

**Date:** 2026-09-26
**Status:** Accepted
**Decision:** `CallMediaEngine::SetCameraEnabled(true)` validates, records the request and the display rotation (read on the caller's UI thread), and returns. The engine's video thread takes a `CameraDeviceLease` (exclusive; opened on the `MediaDeviceArbiter` device thread), creates and configures the local encoder, applies bitrate before each encode, and releases both when the request is withdrawn. A failed open clears the request and is reported once through `TakeCameraFailure`, which the UI polls (same idiom as `TakePendingVideoRefreshStreamIds`) to show the error and withdraw video from the roster.
**Rationale:** The camera used to open synchronously on the UI thread under the engine mutex; routing that through the device thread would make the UI wait behind a mic permission prompt. The video thread already exists per session, so owning the lease there removes cross-thread encoder use (`SetTargetBitrate` from `ApplyAdaptation` raced `Encode`) and the join-under-mutex in the old `CloseCameraLocked` (the video thread takes the mutex to read the send callback). Display rotation is split from transform resolution because iOS reads UIKit (main thread only) and the device thread must never wait on main (UI `Stop` joins the capture thread, which can wait on the device thread).
**Consequence:** `IsCameraEnabled` means "requested", not "frames flowing"; peers may briefly see video advertised before a failed open withdraws it.

---

## L013 — Viewer shape: ticket from the publisher, client-side ladder, direct attach to hops without admission

**Date:** 2026-09-26
**Status:** Accepted (answers CURRENT_STATE's "viewer ticket" question)
**Decision:**
- **Ticket** is fetched 1:1 from the publisher (`ticket_request` over the broadcast RPC; link reach to the publisher). It cannot ride the announce: tickets are bound to one viewer PeerId, tips go to every follower.
- **Admission** is the viewer side of B007 (`BroadcastViewerLadder`, pure): ask the tip hop, then L1 hints; Admit → attach; Redirect → hints first, bounded by the redirect budget, path stamp as loop guard; Refuse / attach failure → next candidate. A hop whose admission RPC cannot run (no broadcast RPC server — every relay before B1, pp-node included) is attached directly as a single-hop relay: admission manages capacity, and frames are end-to-end encrypted (B003), so skipping it exposes nothing.
- **Stream id** = `PublisherStreamIdForIdentity(publisher PeerId)` (`BroadcastPublisherStreamId`) — the tip carries the PeerId, not the Account id calls hash. l5's broadcaster publishes on it.
- **Frames** use `SealMediaRelayFrame` / `OpenMediaRelayFrame` (neutral, `domain/mesh/l4/media_relay`) with context `broadcast-media|<program_id>|<join_handle>`; calls use the same framing under `call-media-sfu|<call_id>` (bytes unchanged).
- **Relay client**: `AmpMediaRelayCoordinator` holds one client session per mesh host, shared with calls. The viewer refuses with a clear reason while a call holds it; per-holder client sessions are a Later item (running both is not needed now — not blocked by design).
**Consequence:** l4 is testable end-to-end only against the test harness until l5 mints tickets (`PutLiveProgramKey`); the hard-lab phase moves to l5, where a real publisher exists.

---

## L014 — Broadcast borrows the call plane's mesh objects until a neutral mesh media plane exists

**Date:** 2026-09-26
**Status:** Superseded by [L015](#l015--a-neutral-meshmediaplane-in-domainmesh-owned-by-the-product-hub-lent-to-calls-and-broadcast) (l8)
**Decision:** The media_relay client, dial registry and circuit/service reach are owned by `CallMediaPlane` (feature/calls) and re-created on mesh (re)start and relay rewires. `ConversationsHub` — which owns both stacks — lends them to broadcast through `CallStack::SharedRelayAttachPorts()` and owns the `BroadcastHub` lifetime around them: built after mesh services start, **dropped before** `ResetRelayClients` / `WireMediaRelayDeps` / `StopMesh`, rebuilt after. Broadcast never includes calls (CI include ban).
**Rationale:** Extracting those objects (plus punch and seed parking, 1.3k lines of `CallMediaPlane`) into `domain/mesh` is the right shape but a refactor of its own; borrowing through a neutral struct with explicit teardown ordering gets a working viewer now without dangling pointers. A rewire stops an active watch (rare: mesh restart / capability change).
**Consequence:** Only one media_relay client session exists per mesh host (L013), so a call attaching replaces the viewer's session — observers carry `Replaced`, and the viewer fails with a clear reason.

---

## L015 — A neutral `MeshMediaPlane` in `domain/mesh`, owned by the product hub, lent to calls and broadcast

**Date:** 2026-09-27
**Status:** Accepted (l8)
**Decision:**
- **What moves.** The media_relay client, dial registry + peer listen book, circuit/service reach (with cold / upgrade punch) and rendezvous parking (warm, reserve, late reserve, park-await, re-park listener) leave `CallMediaPlane` for `domain/mesh/media_plane/MeshMediaPlane`. `CallMediaPlane` keeps call policy only: the call_media Amp transport, `CallMediaBridge`, and the topology's relay deps (`BuildMediaRelayDeps`) built from the neutral objects.
- **Candidates are injected.** `domain/mesh` has no edge to `domain/people`, so hop candidates (rendezvous surface, bootstrap seeds, punch introducers) come through ports filled by the feature that wires the plane, from pure `MeshHopPolicy` helpers.
- **Call concepts become neutral ports.** "Announce the chosen R1" is an on-relay-chosen hook; "signaling punch over call-control" is a last-resort punch port; the account → PeerId note is the caller's (calls wraps `RegisterPeerListenMultiaddrs`).
- **Ownership.** l8a: `CallMediaPlane` owns it (extraction only). l8b: `ConversationsHub` (and `ProductStackHarness`) owns it, lends it to `CallStack` and hands broadcast its `MediaRelayAttachPorts` directly; `CallStack::SharedRelayAttachPorts` goes. The owner sequences every rewire: detach dependents (call topology relay deps, broadcast hub) → rewire / reset the plane → rebind.
**Rationale:** Broadcast borrowing call objects (L014) made the call stack the lifetime authority for a feature that is not a call, and any call-side rewire silently invalidated broadcast. One neutral owner with one rewire sequence removes both the borrow and the class of bug the hard lab found in l7 (a dependent holding a replaced client).
**Consequence:** One media_relay client session per mesh host remains (L013); per-holder sessions stay a Later item and now have a natural home.

