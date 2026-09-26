# Media client layers — design

Status: [CURRENT_STATE.md](CURRENT_STATE.md) · Order: [PHASES.md](PHASES.md) · Why: [DECISIONS.md](DECISIONS.md)

## Target shape

```
feature/calls                                             feature/broadcast
 ├─ CallMediaBridge (1:1 call policy)                      ├─ BroadcastHub
 ├─ CallTopologyController (group joiner policy:           ├─ BroadcastViewerWorkflow
 │    hop ranking, SoftMigrate from 1:1, fan-out,          │    ticket → admit/redirect → receive-only
 │    roster → subscriptions)                              │    attach → re-admit on loss
 └─ CallMediaConnectCoordinator (1:1 bundle)               └─ BroadcasterWorkflow
                                                                capture-only → publish to first relay
        │                         │                                  │
        │            MediaRelayAttachCoordinator  (domain/mesh)  ◄───┘
        │              attach(hop, credentials, stream plan) → quote / AcceptAndAttach → reader;
        │              reattach-on-loss; UI-thread state; generation; hooks
        │                         │
        │                         └──► service reach: ICircuitHopReach::TryEnsureHopReachable (domain/mesh)
        │                                media_relay dialable via protocol-keyed circuit hop (L008)
        └──────────► PeerReachCoordinator  (domain/mesh) — link reach: Connected PeerLink to a peer
                                  │
                      Amp links / circuit / punch

Media (domain/media):  CapturePipeline  (device → encode → send fn)
                       PlaybackPipeline (receive → jitter → decode → mix → device)
                       calls = both (duplex) · broadcaster = capture · viewer = playback
Devices:               DeviceArbiter — leases per kind (mic / camera / speaker); policy exclusive for now
```

## Shared vs separate

| Concern | Shared layer | Calls policy | Broadcast policy |
|---------|--------------|--------------|------------------|
| Reach a node | `PeerReachCoordinator` (peer or hop; `Reach` / `Await`) | 1:1 peer; group hop | first relay; ticket from publisher (if fetched 1:1) |
| Session on a relay | `MediaRelayAttachCoordinator` | group joiner: publish + subscribe N | viewer: receive-only one publisher; broadcaster: publish-only |
| Admission | — | roster / media key | ticket (ML-DSA), admit-or-redirect ladder ([B007](../peer-scoped-broadcast/DECISIONS.md)) |
| Recovery | reattach-on-loss (mechanism) | re-pick hop, attach wait | re-admit, follow redirect |
| Media | capture / playback pipelines | duplex | one-way per role |
| Devices | `DeviceArbiter` leases | mic + speaker (+ camera) | broadcaster: mic (+ camera); viewer: speaker |
| Frame AEAD label | — | `call-media-sfu\|call_id\|…` | own label, e.g. `broadcast\|program_id\|…` |
| Lifecycle / UI entry | — | invite / ring / accept, `CallLifecycle` | announce → watch / stop; Notifications / banner (no ringing) |

**Neutrality test** for a shared layer: it lives outside both features and its API uses none of *call*, *roster*, *program*, *ticket*. Credentials and session ids are opaque.

## Data plane

Same `media_relay` frames through blind relays for both features ([B002](../peer-scoped-broadcast/DECISIONS.md#b002--broadcast-allows-multi-sfu-media-tree-calls-do-not), [B003](../peer-scoped-broadcast/DECISIONS.md)). `MediaRelayTypes` fields named `call_id` become a neutral session id.

**Relays are viewers of their parent** ([L005](DECISIONS.md#l005--a-tree-relay-pulls-upstream-with-the-viewer-client)): a child relay's upstream leg uses the same viewer client (credential → admit-or-redirect → subscribe) as an end viewer; the relay role is "viewer of parent + server for children". One client flow builds the tree.

Later, relay-side only (not blocked by this design): per-program last-keyframe cache for fast join (matters once viewers get video); end-to-end program key so relays forward frames they cannot open (ticket `wrapped_key_b64`).

## Threading

Same rule as the 1:1 split: coordinators and workflows keep state on the UI thread; network completions hop to UI; engine start / stop is UI-only; workers do I/O only. Shared layers check and log off-UI entry (as `CallMediaConnectCoordinator` does).

## Known gaps found while planning (2026-09-26)

- Viewer attaches with `publisher_stream_id = PublisherStreamIdForLocal()` — i.e. as a publisher.
- Viewer shares group SoftMigrate flight / attach-wait state (`flight_.migrate_generation`, `in_flight`).
- `RequestTicket` / `RequestViewerAttach` client RPCs have no product caller (tests only).
- `CallMediaEngine::StartSfu` always starts capture + send + playback ("one engine serves one call"); `StopMeshMedia` stops a mismatched call's engine.
