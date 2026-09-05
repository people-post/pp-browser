# Peer-scoped broadcast — current state

**As of:** 2026-09-05  
**Branch:** `cursor/peer-scoped-announce-broadcast-8d53`

| Spine | Status |
|-------|--------|
| A — calls hop trustworthy | Prerequisite (owned by p2p-av-calls / p2p-mesh); not changed here |
| **B — signed tips without mesh** | **Exit met** — tips + Amp 1:1 + IdentityStore resolve + DM reply |
| **C — tip + live** | **Started** — join plan from live tip (no SoftMigrate/media yet) |
| D — announce helpers | Not started |
| E — CAS replay | Not started |

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
| Tests | `AnnounceLiveJoinTest` in `peer_announce_test.cpp` |

**Still out of scope:** SoftMigrate / media attach, UI join button, epidemic `help_announce`.

See [PROGRAM.md](PROGRAM.md) for sequencing.
