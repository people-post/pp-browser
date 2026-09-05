# Peer-scoped broadcast — current state

**As of:** 2026-09-05  
**Branch:** `cursor/peer-scoped-announce-broadcast-8d53`

| Spine | Status |
|-------|--------|
| A — calls hop trustworthy | Prerequisite (owned by p2p-av-calls / p2p-mesh); not changed here |
| **B — signed tips without mesh** | **In progress** — ML-DSA tips, local publisher, rpc codec; Amp fan-out not wired |
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
| Tests | `tests/peer_announce_test.cpp` (6 cases) |

**Signing:** tips use **device ML-DSA-65** (PeerId-bound), same family as IdentityStore — not Ed25519.

**Still out of scope this slice:** Amp `OpenChannel` fan-out service, epidemic `help_announce`, UI, live media session join.

See [PROGRAM.md](PROGRAM.md) for sequencing.
