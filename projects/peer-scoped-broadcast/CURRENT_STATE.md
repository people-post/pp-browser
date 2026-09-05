# Peer-scoped broadcast — current state

**As of:** 2026-09-05  
**Branch:** `cursor/peer-scoped-announce-broadcast-8d53`

| Spine | Status |
|-------|--------|
| A — calls hop trustworthy | Prerequisite (owned by p2p-av-calls / p2p-mesh); not changed here |
| **B — signed tips without mesh** | **In progress** — ML-DSA tips, Amp 1:1, IdentityStore publisher + peer_id key resolve |
| C — tip + live | Not started |
| D — announce helpers | Not started |
| E — CAS replay | Not started |

## Spine B landed

| Piece | Path |
|-------|------|
| Types / caps / heartbeat constants | `src/domain/messaging/PeerAnnounceTypes.h` |
| Topic id, canonical sign bytes, JSON, **ML-DSA-65** sign/verify, heartbeat timing | `PeerAnnounceCodec.*` |
| In-memory verify + seq/epoch dedup feed | `PeerAnnounceFeed.*` |
| Local publisher (seq/epoch, go-live/end, live heartbeat) | `PeerAnnouncePublisher.*` |
| Tip push/ack JSON + `/pp-browser/rpc/peer-announce/1.0.0` | `PeerAnnounceRpcCodec.*`, `IDirectMessageClient.h`, L4 table |
| Amp 1:1 tip transport | `feature/conversations/AmpPeerAnnounceService.*` (OpenChannel + feed ingest) |
| Mesh advertise | `MeshHost` includes `kRpcPeerAnnounceProtocolId` |
| Device publisher + inbound key resolve | `PeerAnnounceKeyResolve.*`; `MeshMessagingService` wires IdentityStore device ML-DSA + `PeerSigningKeyStore` kind `peer_id`; `PublishAndPushAnnounce` |
| Tests | `peer_announce_test.cpp` (incl. key resolve); `amp_peer_announce_service_test.cpp` (round-trip, unknown key ack, store-backed resolve) |

**Signing:** tips use **device ML-DSA-65** (PeerId-bound), same family as IdentityStore — not Ed25519. Account-kind signing keys are **not** used for tip verify.

**Still out of scope this slice:** epidemic `help_announce`, UI, tip→live (Spine C), full MeshMessaging integration tests.

See [PROGRAM.md](PROGRAM.md) for sequencing.
