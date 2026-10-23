# CodedFailure rollout (history and open items)

The contract: [docs/contracts/CODED_FAILURE.md](../../docs/contracts/CODED_FAILURE.md). This file
keeps how it was rolled out and what is still open.

## Rollout

### Phase 1 — Mesh port (done)

- [x] `IChatPeerLinks::LinkRoe` / `ChannelRoe` → `CodedRoe`
- [x] Identity map from `PeerLinkManager` preserving code + message
- [x] `IsAssociationNotReady`, `IsDialInBackoff`, `IsEndpointNotRegistered`
- [x] Tests: `mesh_ports_test.cpp`
- [x] [AMP-LINK-ERRORS.md](AMP-LINK-ERRORS.md) port alias note

### Phase 2 — L4 owning layers (done)

Each coordinator gets a **local `Err` enum**, `Failure` / `Roe` aliases, and
**`WrapLinkFailure(const …::Failure&)`** (name may vary). Wrap at every
failure exit that originated from association / channel open / nested
carrier — not string copy alone.

| Module | Primary files | Suggested `Err` themes | Status |
|--------|---------------|------------------------|--------|
| Dial-back | `reachability/dial_back/` (`DialBackTypes`, `client/DialBackClient`, `serve/DialBackServer`) | `NotStarted`, `EndpointNotRegistered`, `LinkFailed`, `Timeout`, `ChannelFailed`, `ProtocolError`, `Generic` | **done** — wraps `PeerLinkManager::Failure` |
| DHT | `dht/client/DhtClient.{h,cpp}` (via `AmpDhtProtocol`) | `NotStarted`, `LinkFailed`, `Timeout`, `ChannelFailed`, `NotFound`, `Generic`, … | **done** — wraps `PeerLinkManager::Failure`; `FindPeer` returns `FindPeerRoe` |
| Circuit tunnel | `l4/circuit/client/CircuitClientCoordinator.{h,cpp}`, `serve/CircuitRelayServer.{h,cpp}` | `CircuitTunnelErr` | **done** (pp-cpp-mesh) — the relay's refusal is a wire `code` |
| Circuit reach | `reach/AmpCircuitHopReach`, `ICircuitHopReach` (`MeshReachPorts.h`) | `CircuitReachErr` | **done** (pp-cpp-mesh) |
| Punch walk | `reach/PunchIntroducerWalk`, `PunchErr` | `PunchIntroducerWalk::Err` | **done** (pp-cpp-mesh) — the introducer's refusal is a wire `code` |
| Peer reach | `reach/PeerReachCoordinator` | `PeerReachCoordinator::Err` | **done** (pp-cpp-mesh); `IDialRegistry` passes link codes through |
| Media relay | `l4/media_relay/client/MediaRelayClientCoordinator`, `IMediaRelayClient`, `media_plane/MediaRelayAttach` | `MediaRelayErr` | **done** (pp-cpp-mesh) — the hop's refusal is a wire `code` |
| Call-media leg | `domain/call_media/CallMediaLegCoordinator`, `ICallMediaTransport` | `CallMediaErr` | **done** — connects; migrations still report text |

The mesh's code tables and wire codes: pp-cpp-mesh `docs/ERRORS.md`.

**Note:** Dial-back / DHT currently own `PeerLinkManager&` directly (mesh-internal), so
`WrapLinkFailure` takes `PeerLinkManager::Failure`. Feature-facing L4 that goes through
`IChatPeerLinks` wraps that port’s `Failure` instead — same numeric child table.

**Phase 2 checklist (per module):**

1. Add `enum class Err` + `using Failure = CodedFailure<Err>` (header or colocated `*Errors.h`).
2. Implement `WrapLinkFailure` with `switch` on the **immediate child's `Err` only**
   (`PeerLinkManager::Err` or `IChatPeerLinks::Err` — never ADP / PeerLink).
3. Replace `Error(link.error().message)` / opaque forwards with wrapped `Failure`.
4. Export `Is*` helpers only where multiple call sites branch the same way.
5. Unit/compose test: assert **`GetCode()`** at L4 boundary; keep message substring optional.
6. Document stable L4 codes in this file or a short module note when stabilized.

**Explicitly out of Phase 2:** codecs, SQLite, HTTP clients, `AppError` migration.

### Phase 3 — Feature Amp services

- [x] Retry / backoff decisions read codes, not message text. No message-substring branch is left on
      a transport path: group-membership controls dispatch on their control type
      (`RelayReceivePipeline`), and a call's clean stream close is `CallMediaErr::StreamClosed`,
      read by the call bridge (it matched "stream closed" / "read_eof").
- [x] No code strips at `Error(child.message)`: `CallMediaConnectCoordinator` wraps a peer reach
      failure (`WrapReachFailure`) and finishes with `CallMediaRoe`; the broadcast viewer's
      `reach_peer` port passes the reach's own result through; `ICallMediaTransport::on_failed`
      carries the `CallMediaFailure`. `ConversationsHub::RequestCircuitBridgePreferred` (unused)
      is gone.
- [x] `CallMediaConnectCoordinator` decides "dial a fresh link next time" by `CallMediaErr::StaleLink`.
- [x] Chat send notices by code: the direct send reports `DirectSendErr` (`IDirectMessageClient`;
      `AmpDirectChatTransport::WrapLinkFailure`), the relay send `AppError` codes
      (`HttpRelayClient::Send`: rate limited, not configured, HTTP status; network failures were
      coded already), and `lib/messaging/DeliveryNotice` maps them (`SendFailureFromDirect`,
      `SendFailureFromRelay`). With no relay route, the notice says why the direct send failed.
- [ ] `AmpChatHistoryTransport`, `AmpChatBlobTransport`: still text. Their callers
      (`ChatSyncWorkflow`, `AttachmentFetch`, `AttachmentClient`) fall back to the relay / CDN on
      any peer failure, so nothing branches yet; code them (as `IDirectMessageClient`) when a caller
      or the UI mapping (Phase 4) needs to tell the failures apart.

### Phase 4 — UI boundary

- [ ] Map terminal failures to **`AppError`** for display; codes drive logic, catalog drives copy.

### Phase 5 — pp-cpp-amp hardening (optional)

- [ ] `ChannelMux::Err` + wrap at `PeerLinkManager` where L3 rejection needs discrimination.
- [ ] Dedupe `CodedFailure.h` copies (amp L1/link vs `pbr::` vs future `pp_common`).
