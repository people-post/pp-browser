# Peer-scoped broadcast — current state

**As of:** 2026-09-30
**Branch:** `refactor/mesh-connectivity` (video levels, B009)

| Spine | Status |
|-------|--------|
| A — calls hop trustworthy | Prerequisite (owned by p2p-av-calls / p2p-mesh); not changed here |
| **B — signed tips without mesh** | **Exit met** — tips + Amp 1:1 + IdentityStore resolve + DM reply |
| **C — tip + live** | **In progress** — viewer moved to `feature/broadcast` (2026-09-26, [B008](DECISIONS.md), [media-client-layers l4](../media-client-layers/PHASES.md)): tip → ticket → ladder → receive-only attach → playback; broadcaster `GoLive` / `EndLive` (media-client-layers l5); the call-side arm/accept path was removed in l6. No UI entry yet |
| D — announce helpers | Not started |
| E — CAS replay | Not started |
| **F — media tree** | **B0/B1 + Amp handlers** — codecs + `AmpBroadcastTransport`; viewer / broadcaster in `feature/broadcast` |

## Spine B landed

| Piece | Path |
|-------|------|
| Types / caps / heartbeat constants | `src/domain/messaging/PeerAnnounceTypes.h` |
| Topic id, canonical sign bytes, JSON, **ML-DSA-65** sign/verify, heartbeat timing | `PeerAnnounceCodec.*` |
| In-memory verify + seq/epoch dedup feed | `PeerAnnounceFeed.*` |
| Local publisher (seq/epoch, go-live/end, live heartbeat) | `PeerAnnouncePublisher.*` |
| Tip push/ack JSON + `/pp-browser/rpc/peer-announce/1.0.0` | `PeerAnnounceRpcCodec.*`, protocol id in DM client headers, L4 table |
| Amp 1:1 tip transport | `feature/conversations/AmpPeerAnnounceTransport.*` |
| Mesh advertise | `MeshHost` includes peer-announce protocol id |
| Device publisher + inbound key resolve | `PeerAnnounceKeyResolve.*`; `MeshDeliveryOrchestrator` wires IdentityStore device ML-DSA + `PeerSigningKeyStore` kind `peer_id`; `PublishAndPushAnnounce` |
| DM reply path (no in-topic speak) | `AnnounceOverlayReply.*` (`PlanAnnounceDmReply`); `MeshDeliveryOrchestrator::ReplyToAnnouncePublisher` |
| Tests | `peer_announce_test.cpp` (codec/feed/publisher/rpc/key resolve/DM plan); `amp_peer_announce_service_test.cpp` |

**Signing:** tips use **device ML-DSA-65** (PeerId-bound). Account-kind signing keys are **not** used for tip verify.

**Spine B still out of scope:** epidemic `help_announce`, UI chrome, full MeshMessaging integration tests.



## Spine F / B0 started (media key ticket) + B007 discovery locked

| Piece | Path |
|-------|------|
| Join ticket types + mint/verify/JSON | `BroadcastJoinTicket.*` |
| Extract media key (memory only; no `CallMediaKeyStore`) | `ExtractBroadcastMediaKey` |
| Tests | `broadcast_join_ticket_test.cpp` |
| Recursive ladder discovery (spec) | [MEDIA_TREE.md § B007](MEDIA_TREE.md#recursive-ladder-discovery-b007), [DECISIONS B007](DECISIONS.md#b007--recursive-whitelist-ladder-discovery-admit-or-redirect) |

**B003 locked:** encrypt-once AEAD mandatory; every hop must forward opaque blobs (no cleartext escape).  
**B007 locked:** tip names online L1 whitelist only; hops admit-or-redirect; new relays may win slots and demote piped viewers one rung down.

| Ladder admit/redirect + slot-win (pure) | `BroadcastLadderLogic.*` + `broadcast_ladder_logic_test.cpp` |
| Tip L1 hints | `PeerAnnounceTip::l1_hop_peer_ids` (codec omit-empty) |

| Broadcast RPC codec | `BroadcastRpcCodec.*` — ticket_request/response, viewer_attach(_result), relay_slot_win(_result); `/pp-browser/rpc/broadcast/1.0.0` |
| Session shape | None — watching is not a call session. `CallSessionKind::Broadcast` is a legacy parsed value only; `AcceptInvite` refuses it |

| Amp broadcast RPC | `feature/conversations/AmpBroadcastTransport.*` — ticket mint, viewer_attach, relay_slot_win over `/pp-browser/rpc/broadcast/1.0.0` |
| Mesh advertise + wire | `MeshHost` advertises broadcast protocol; `MeshDeliveryOrchestrator` starts service + device key resolvers |
| Tests | `amp_broadcast_service_test.cpp` (ticket / admit / slot-win round-trips) |

| Viewer / broadcaster | `feature/broadcast/` — `BroadcastHub` (owned by `ConversationsHub`), `BroadcastViewerWorkflow`, `BroadcasterWorkflow`; facade `WatchLiveAnnounce` / `GoLive` / `EndLive` ([media-client-layers](../media-client-layers/CURRENT_STATE.md)) |

**Still out of scope for this slice:** watch / go-live UI, live redirect/slot-win media fan-out runtime.

## Spine C started (slice 0)

| Piece | Path |
|-------|------|
| Tip → watch target (Live program, publisher / program / join handle) | `BroadcastWatchTargetFromTip` in `feature/broadcast/BroadcastViewerWorkflow.*` |
| Optional tip `hop_peer_id` / `l1_hop_peer_ids` → ladder hops | `PeerAnnounceTypes` / codec / publisher; viewer ladder |
| Watch entry | facade `WatchLiveAnnounce` → `BroadcastHub::WatchLive` (the call-shaped arm/accept path was removed in media-client-layers l6) |
| Tests | `broadcast_viewer_workflow_test.cpp`, `broadcast_viewer_compose_test.cpp` |

**Domain wire landed (bare minimum, schema v1 additive):** tip `kind` / `viewer_peer_id` / `viewer_msg_id`; `AnnounceOverlayReply` + rate helpers; `AnnounceNotificationInbox`; feed isolates `live_chat` from program `Latest()`; Mesh `ReplyToAnnounceOverlay` / `PublishLiveChatFromOverlay`; Amp `SetOnTipIngested` → inbox upsert.

**Still out of scope:** Notifications/banner **UI chrome**, join button chrome, epidemic `help_announce`. Media attaches only when the tip or ticket names a hop.

**Product UX:** Discovery ≠ call ring; Notifications + optional live banner (domain inbox ready; UI later); Watch reuses join API without ringtone; Private vs On-screen replies (publisher-signed overlay tips + rate limit / block). See [DESIGN.md](DESIGN.md#product-pickup-ux--not-call-ringing).

See [PROGRAM.md](PROGRAM.md) for sequencing.

## Video levels (B009) — V1–V4 landed

| Piece | Path |
|-------|------|
| Channel = track kind + level | `common/media/MediaChannel.h`; [MEDIA_CHANNELS.md](../../docs/contracts/MEDIA_CHANNELS.md) |
| Relay policy + negotiation (offer / answer in the quote; ingest drop) | `MediaRelayVideoLevels.*`, `MediaRelayServer` (`SetVideoPolicy`), config `mesh.media_relay_video` |
| Broadcaster offers device levels, publishes the answered one, tip lists it | `BroadcasterWorkflow` (`BroadcastLiveRequest::video`), `VideoLevelProfile.h`, `CallMediaEngine::SetVideoLevel`, `PeerAnnounceTip::video_levels` (signed) |
| Viewer takes the nearest published level | `ChooseWatchVideoLevel`, `BroadcastViewerWorkflow` |
| Tests | `media_relay_video_levels_test`, `media_relay_server_client_test` (answer + ingest drop), `broadcaster_workflow_test`, `broadcast_viewer_workflow_test`, `peer_announce_test` |

**Open:** V5 (two encoders), relay capability advertised for the user's choice, watch / go-live UI.

