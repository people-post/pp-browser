# Current state — ADP / AMP

**As of:** 2026-08-31

## Landed (L1)

- Project docs + ADRs A001–A020
- `src/base/adp/` → `pp_base_adp` (Asio-free)
- Wire v1 + HMAC-SHA256-128, best-effort + reliable, path migrate, OsUdp
- `pp_browser_adp_test` (26 tests)
- Contract: [`docs/contracts/ADP.md`](../../docs/contracts/ADP.md)

## Landed (stack spec — D0)

- [STACK.md](STACK.md) — four-layer AMP model
- [`docs/contracts/AMP-SESSION.md`](../../docs/contracts/AMP-SESSION.md) — L2 MSH + full AEAD
- [`docs/contracts/AMP-CHANNEL.md`](../../docs/contracts/AMP-CHANNEL.md) — L3 mux, ch0, fragmentation
- [PHASES.md](PHASES.md) — D1–D9 migration checklist

## Transitional (legacy TCP path)

- **Opus side-path:** `CallMediaAdpPath` + TCP-hello `adp_*` + `CallMediaAdpDogfood.h` (A008–A011)
- **Production mesh:** still TCP + Noise + Yamux via `Libp2pHost`
- Delete transitional Opus path when **D6** ships

## Landed (L2 — D1)

- `src/base/mesh/session/` → `pp_base_mesh_session`
- MSH v1 handshake (ML-KEM + ML-DSA identity bind), session key derivation
- `Session` seal/open (XChaCha20-Poly1305 + AAD), rekey
- `pp_browser_amp_session_test` (8 tests, green)

## Landed (L3 — D2)

- `src/base/mesh/channel/` → `pp_base_mesh_channel`
- L3 wire codec, `ChannelMux`, `ChannelSession`, channel 0 capability plane
- FRAG reassembly for large payloads; QoS class → ADP Reliable/BestEffort
- D3 fragmentation edge tests (reorder, loss, dup, timeout)
- `pp_browser_amp_channel_test` (14 tests, green)

## Landed (link layer — D4)

- `src/base/mesh/link/` → `pp_base_mesh_link`
- ADP multiaddr parse/format, MSH-over-ADP (chunked), `PeerLinkManager`, `MeshPump`, **`MeshRuntime`**
- PeerId from MSH identity; inbound link adopt/rekey to registered alias
- `EnsureAssociation` + `OpenChannel` over `MemoryDatagramIo`
- `pp_browser_amp_link_test` (5 tests, green)

## Landed (L4 chat — D5)

- `AmpDirectChatService` + `AmpChatHistoryService` — `/pp-browser/chat/1.0.0` and `/pp-browser/chat-history/1.0.0` over `ChannelSession` / `PeerLinkManager::OpenChannel`
- `ChannelMux::SetProtocolHandler` + `PeerLinkManager::SetProtocolHandler` for inbound L4 dispatch
- `pp_browser_feature_messaging_test` — `AmpDirectChatServiceTest`, `AmpChatHistoryServiceTest` (parallel stack; production still libp2p)

## Landed (L4 call-media — D6)

- `CallMediaLegCoordinator` — `/pp-browser/call-media/1.0.0` **`call_id`-keyed channel bundle** on `MeshRuntime`: role-tagged outbound/inbound control + media; pure admit helpers in `CallMediaBundleLogic` ([A021](DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime))
- Dual-dial glare: higher base58 PeerId keeps outbound; lower yields and adopts inbound
- L3 remote terminal → session `on_closed` (`peer_close` / `peer_reset`); `ChannelSession::CloseQuiet` for provisional roles
- Optional `WorkerPost` for inbound hello (matches libp2p worker-lane stall tests)
- `pp_browser_p2p_test` — `CallMediaBundleLogicTest` + 9× `CallMediaLeg*` cases
- Shared AMP test harness: `mesh_test_harness.h` + `MeshRuntime`

## Landed (L4 circuit — D7a / A022)

- `CircuitBundleLogic` — pure admit / ack / close decisions
- `CircuitTunnelCoordinator` — non-blocking `StartBridge` on `MeshRuntime` (callbacks + `PostToIo`; no nested `Pump`)
- `ChannelBridge` — io-thread DATA splice armed after handshake
- Bridge JSON + admission parity with libp2p circuit; parallel stack only ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol))
- `pp_browser_p2p_test` — `CircuitBundleLogicTest` + `CircuitTunnelCoordinatorTest`

## Landed (L4 media-relay — D7b / A022)

- `MediaRelayBundleLogic` — admit / ack / default quote helpers (reuses `MediaRelayAttachSm` + `MediaRelayLogic`)
- `AmpMediaRelayCoordinator` — non-blocking `StartQuote` / `StartAttach` on `MeshRuntime`
- `pp_browser_p2p_test` — `MediaRelayBundleLogicTest` + `AmpMediaRelayCoordinatorTest`
- Fan-out SoftMigrate / MeshHost wire deferred

## Landed (D8 ch0 — partial)

- After MSH, dialer opens channel 0 (`/pp-browser/amp-capability/1.0.0`); peer replies with local caps
- `PeerLinkManager::{SetLocalListenMultiaddrs,SetAdvertisedProtocols,SetCapabilityHandler,PreferredMultiaddr}`
- `PeerLink::RemoteCapability()` stores peer Identify replacement payload
- **Ingest:** first valid ADP listen multiaddr from remote caps upserted under authenticated PeerId (aliases refreshed)
- `ChannelMux::SendCapabilityOffer` queues DATA until OpenAck (async ADP); `OpenOutbound` claims pending data handlers
- `pp_browser_amp_link_test` — `CapabilityExchangeAfterAssociation`, `CapabilityIngestEnablesPeerIdDial`
- Still open: dial-back protocol on AMP, mDNS on ADP multiaddrs, listen policy

## Landed (D9 building block)

- `AmpStack` — owns `DatagramIo` + `Endpoint` + `MeshRuntime` for one local peer
- **`MeshHost` parallel Amp** ([A023](DECISIONS.md#a023--meshhost-may-own-ampstack-in-parallel-same-device-keys)): `enable_amp_stack` / `AttachAmpStack`; soft-fail Amp; `Tick` pumps Amp
- **Product wiring:** `libp2p.enable_amp_stack` (default **true**) → MessagingHub + pp-node; `TickLibp2p` calls `mesh_->Tick()` so Amp pumps
- `pp_browser_p2p_test` — `MeshHostAmpTest.AttachAmpStackParallelNoLibp2p`

## Landed (D9 step 3 — chat/history single entry)

- **Composition cutover:** when `MeshHost::Amp()` is up, `P2pMessagingService` constructs `AmpDirectChatService` + `AmpChatHistoryService` only (no dual chat handlers; [A020](DECISIONS.md#a020--single-transport-entry-per-protocol))
- Blob / call-media / circuit / dial-back remain on libp2p
- ADP endpoints registered from contacts, ch0 ingest, Identify Amp listen push, and LAN mDNS TXT `amp_udp=`
- Without an ADP multiaddr, direct chat falls back to relay (TCP-only contacts)

## Landed (D9 step 4 — call-media single entry)

- **`ICallMediaTransport`** — shared product entry; `CallMediaDirectService` (libp2p) or **`CallMediaAmpTransport`** → `CallMediaLegCoordinator` (Amp)
- `CallStack::OnMeshServicesStarted` picks Amp when `MeshHost::Amp()` is up ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol))
- `kCallMediaAdpOpusDogfood` set **false** (legacy TCP-hello Opus side-path idle; delete with D9 step 7)
- PreferLocal advertise list includes Amp listen multiaddr when present

## Plan adjustments (2026-08-31)

| Change | Why |
|--------|-----|
| **Do not block MeshHost Amp attach on AMP dial-back / mDNS** | Ownership is independent of reachability chrome; libp2p DialBack/Identify still cover probes until cutover |
| **Shared device ML-DSA keys required for `enable_amp_stack`** | One PeerId across stacks ([A023](DECISIONS.md#a023--meshhost-may-own-ampstack-in-parallel-same-device-keys)) |
| **`MeshHost::Tick` must Pump Amp** | Idle UDP stack otherwise never completes MSH/ch0 |
| **Amp start is soft-fail** | Parallel phase must not take down libp2p mesh |
| **Keep L4 flip order: chat → call-media → circuit/media-relay** | Unchanged; SoftMigrate fan-out stays after circuit/media AMP is the single entry |
| **AMP dial-back is optional until Identify/TCP teardown** | Prefer ch0 addr ingest + existing libp2p DialBack during parallel phase |
| **Chat+history flip together** | Shared Amp address book / reachability; blob stays libp2p |

## Next (implementation)

1. **D9** — circuit/media-relay product SoftMigrate
2. **D8 optional** — AMP dial-back when retiring libp2p Identify
3. **D9** — delete dogfood headers + TCP call-media path; update NETWORKING / CALLS

### D9 cutover checklist

Do **not** dual `if (amp)` in product paths ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol)). Flip one protocol family at a time behind MeshHost composition, then delete the libp2p path.

| Step | Action | Notes |
|------|--------|--------|
| 1 | Own `AmpStack` inside `MeshHost` (parallel) | **Done** — product `enable_amp_stack` default true ([A023](DECISIONS.md#a023--meshhost-may-own-ampstack-in-parallel-same-device-keys)) |
| 2 | Wire ch0 listen addrs + advertised L4 protocol list from hosting posture | **Done** — `ApplyAmpAdvertisement`; ingest on PeerLinkManager |
| 3 | Swap chat + history to `AmpDirectChatService` / `AmpChatHistoryService` | **Done** — MessagingHub composition; mDNS `amp_udp` + Identify Amp MA |
| 4 | Swap call-media to `CallMediaLegCoordinator`; drop `kCallMediaAdpOpusDogfood` | **Done** — `CallMediaAmpTransport` + `ICallMediaTransport`; dogfood gate **false** |
| 5 | Swap circuit + media-relay to AMP coordinators | SoftMigrate / MeshHost fan-out |
| 6 | Stop starting libp2p Identify / TCP listen for mesh | Keep PeerId crypto helpers; AMP dial-back/mDNS if still needed |
| 7 | Delete dogfood + TCP-hello Opus; update NETWORKING / CALLS / LIBP2P_STREAMS | |

See [PHASES.md](PHASES.md) for full ordering.
