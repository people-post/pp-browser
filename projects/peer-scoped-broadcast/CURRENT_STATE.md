# Peer-scoped broadcast — current state

**As of:** 2026-09-05  
**Branch:** `cursor/peer-scoped-announce-broadcast-8d53`

| Spine | Status |
|-------|--------|
| A — calls hop trustworthy | Prerequisite (owned by p2p-av-calls / p2p-mesh); not changed here |
| **B — signed tips without mesh** | **Exit met** — tips + Amp 1:1 + IdentityStore resolve + DM reply |
| **C — tip + live** | **In progress** — plan + arm + accept (SFU via hop_peer_id; no SoftMigrate/1:1) |
| D — announce helpers | Not started |
| E — CAS replay | Not started |
| **F — media tree** | **Spec only** — [MEDIA_TREE.md](MEDIA_TREE.md), [DECISIONS.md](DECISIONS.md) B001–B006, [PHASES.md](PHASES.md); code after C dogfood |

## Spine B landed

| Piece | Path |
|-------|------|
| Types / caps / heartbeat constants | `src/domain/messaging/PeerAnnounceTypes.h` |
| Topic id, canonical sign bytes, JSON, **ML-DSA-65** sign/verify, heartbeat timing | `PeerAnnounceCodec.*` |
| In-memory verify + seq/epoch dedup feed | `PeerAnnounceFeed.*` |
| Local publisher (seq/epoch, go-live/end, live heartbeat) | `PeerAnnouncePublisher.*` |
| Tip push/ack JSON + `/pp-browser/rpc/peer-announce/1.0.0` | `PeerAnnounceRpcCodec.*`, protocol id in DM client headers, L4 table |
| Amp 1:1 tip transport | `feature/conversations/AmpPeerAnnounceService.*` |
| Mesh advertise | `MeshHost` includes peer-announce protocol id |
| Device publisher + inbound key resolve | `PeerAnnounceKeyResolve.*`; `MeshMessagingService` wires IdentityStore device ML-DSA + `PeerSigningKeyStore` kind `peer_id`; `PublishAndPushAnnounce` |
| DM reply path (no in-topic speak) | `AnnounceDmReply.*` planner; `MeshMessagingService::ReplyToAnnouncePublisher` |
| Tests | `peer_announce_test.cpp` (codec/feed/publisher/rpc/key resolve/DM plan); `amp_peer_announce_service_test.cpp` |

**Signing:** tips use **device ML-DSA-65** (PeerId-bound). Account-kind signing keys are **not** used for tip verify.

**Spine B still out of scope:** epidemic `help_announce`, UI chrome, full MeshMessaging integration tests.


## Spine C started (slice 0)

| Piece | Path |
|-------|------|
| Live-join plan from tip (`call_id` = `join_handle`) | `AnnounceLiveJoin.*`; `MeshMessagingService::PlanLiveJoinFromAnnounceTip` / `PlanLiveJoinFromStoredAnnounce` |
| Arm pending invite + ringing session from plan | `AnnounceLiveJoinHandoff.*`; `CallSessionManager::ArmJoinFromLiveAnnounce` (no SoftMigrate/media) |
| UI / facade tip→arm entry points | `CallUiBackend::ArmJoinFromLiveAnnounce`; `ConversationsFacade::ArmLiveJoinFromAnnounceTip` / `ArmLiveJoinFromStoredAnnounce` |
| Optional tip `hop_peer_id` → session `sfu_hint` | `PeerAnnounceTypes` / codec / publisher; plan + handoff carry through |
| Accept without SoftMigrate / 1:1 media | `CallTopologyController::OnAnnounceViewerJoined`; `CallSessionManager::AcceptLiveAnnounceJoin`; facade `JoinLiveAnnounceFromTip` |
| Tests | `AnnounceLiveJoinTest` in `peer_announce_test.cpp` |

**Domain wire landed (bare minimum, schema v1 additive):** tip `kind` / `viewer_peer_id` / `viewer_msg_id`; `AnnounceOverlayReply` + rate helpers; `AnnounceNotificationInbox`; feed isolates `live_chat` from program `Latest()`; Mesh `ReplyToAnnounceOverlay` / `PublishLiveChatFromOverlay`; Amp `SetOnTipIngested` → inbox upsert.

**Still out of scope:** SoftMigrate for announce viewers, Notifications/banner **UI chrome**, join button chrome, epidemic `help_announce`. Media attaches only when `hop_peer_id`/`sfu_hint` is present.

**Product UX:** Discovery ≠ call ring; Notifications + optional live banner (domain inbox ready; UI later); Watch reuses join API without ringtone; Private vs On-screen replies (publisher-signed overlay tips + rate limit / block). See [DESIGN.md](DESIGN.md#product-pickup-ux--not-call-ringing).

See [PROGRAM.md](PROGRAM.md) for sequencing.
