# Peer-scoped broadcast — current state

**As of:** 2026-09-05  
**Branch:** `cursor/peer-scoped-announce-broadcast-8d53`

| Spine | Status |
|-------|--------|
| A — calls hop trustworthy | Prerequisite (owned by p2p-av-calls / p2p-mesh); not changed here |
| **B — signed tips without mesh** | **Started** — domain types/codec/feed + unit tests |
| C — tip + live | Not started |
| D — announce helpers | Not started |
| E — CAS replay | Not started |

## Spine B landed (thin)

| Piece | Path |
|-------|------|
| Types / caps / heartbeat constants | `src/domain/messaging/PeerAnnounceTypes.h` |
| Topic id, canonical sign bytes, JSON, Ed25519 sign/verify, heartbeat timing | `PeerAnnounceCodec.*` |
| In-memory verify + seq/epoch dedup feed | `PeerAnnounceFeed.*` |
| Tests | `tests/peer_announce_test.cpp` (4 cases) |

**Still out of scope for this slice:** rpc fan-out, epidemic `help_announce`, UI, IdentityStore wiring, live media session join.

See [PROGRAM.md](PROGRAM.md) for sequencing.
