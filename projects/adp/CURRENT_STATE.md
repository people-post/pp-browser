# Current state — ADP / AMP

**As of:** 2026-09-01

## Landed (L1)

- Project docs + ADRs A001–A020
- `src/lib/amp/L1/` → `pp_base_adp` (Asio-free)
- Wire v1 + HMAC-SHA256-128, best-effort + reliable, path migrate, OsUdp
- `pp_browser_adp_test` (40 tests)
- Contract: [`docs/contracts/ADP.md`](../../docs/contracts/ADP.md)

## Landed (stack spec — D0)

- [STACK.md](STACK.md) — four-layer AMP model
- [`docs/contracts/AMP-SESSION.md`](../../docs/contracts/AMP-SESSION.md) — L2 MSH + full AEAD
- [`docs/contracts/AMP-CHANNEL.md`](../../docs/contracts/AMP-CHANNEL.md) — L3 mux, ch0, fragmentation
- [PHASES.md](PHASES.md) — D1–D9 migration checklist

## Transitional (legacy TCP path)

- **Deleted (A017):** product TCP L4 sources + vendored libp2p Host/TCP/Yamux/Noise/QUIC/protocol trees
- **Retained in `src/lib/libp2p`:** PeerId + `keys_wire` (+ multihash/SHA/log) for ML-DSA identity
- **Next:** D8 listen-policy polish; org seed `amp_udp_port=443` deploy

## Landed (L2 — D1)

- `src/lib/amp/L2/` → `pp_base_mesh_session`
- MSH v1 handshake (ML-KEM + ML-DSA identity bind), session key derivation
- `Session` seal/open (XChaCha20-Poly1305 + AAD), rekey
- `pp_browser_amp_session_test` (12 tests, green)

## Landed (L3 — D2)

- `src/lib/amp/L3/` → `pp_base_mesh_channel`
- L3 wire codec, `ChannelMux`, `ChannelSession`, channel 0 capability plane
- FRAG reassembly for large payloads; QoS class → ADP Reliable/BestEffort
- D3 fragmentation edge tests (reorder, loss, dup, timeout)
- `pp_browser_amp_channel_test` (16 tests, green)

## Landed (link layer — D4)

- `src/lib/amp/link/` → `pp_base_mesh_link`
- ADP multiaddr parse/format, MSH-over-ADP (chunked), `PeerLinkManager`, `MeshPump`, **`MeshRuntime`**
- PeerId from MSH identity; inbound link adopt/rekey to registered alias
- `EnsureAssociation` + `OpenChannel` over `MemoryDatagramIo`
- `pp_browser_amp_link_test` (9 link unit tests, green)
- `pp_browser_amp_integration_test` (15 Tier B tests, green)
- Shared harness: `src/lib/amp/tests/support/` (AMP tests); L4 compose: `src/base/p2p/tests/support/`

## Landed (L4 chat — D5)

- `AmpDirectChatService` + `AmpChatHistoryService` — `/pp-browser/chat/1.0.0` and `/pp-browser/chat-history/1.0.0` over `ChannelSession` / `PeerLinkManager::OpenChannel`
- `ChannelMux::SetProtocolHandler` + `PeerLinkManager::SetProtocolHandler` for inbound L4 dispatch
- `pp_browser_feature_messaging_test` — `AmpDirectChatServiceTest`, `AmpChatHistoryServiceTest` (parallel stack; production still libp2p)

## Landed (L4 call-media — D6)

- `CallMediaLegCoordinator` — `/pp-browser/call-media/1.0.0` **`call_id`-keyed channel bundle** on `MeshRuntime`: role-tagged outbound/inbound control + media; pure admit helpers in `CallMediaBundleLogic` ([A021](DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime))
- Dual-dial glare (L4): higher base58 PeerId keeps outbound; lower yields and adopts inbound
- L3 remote terminal → session `on_closed` (`peer_close` / `peer_reset`); `ChannelSession::CloseQuiet` + `ReleaseHandlers` (dtor / provisional slots); Bind keeps `shared_ptr` for dispatch so TearDown-from-callback is safe ([A027](DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy))
- `AdoptClientChannel` must not touch `Session` after `sessions.erase` (parent-only destroy / no use-after-erase)
- Bundle resolves `PeerLink` via `peer_key` at use sites (no long-lived raw link pointer)
- SoftMigrate audio reopen gated to Android settle (`CaptureReopenSettleDelayMs() > 0`)
- Connect timeout does not `Detach()` when inbound already `IsActive()`
- Optional `WorkerPost` for inbound hello (matches libp2p worker-lane stall tests)
- `pp_browser_p2p_test` — `CallMediaBundleLogicTest` + `CallMediaLeg*` cases
- Shared AMP test harness: `src/lib/amp/tests/support/mesh_test_harness.h` + `MeshRuntime`

### LAN dogfood checklist (call-media / A026)

1. Accept → Connect ok → InCall with `rx_frames > 0`, no abort in the first seconds
2. SoftMigrate / SFU: `rx_frames` keep climbing (not stall with rising `rx_age_ms`) — Amp mesh pump ~5ms
3. Leave without malloc / double-free
4. Logs: one PeerLink alias for the remote during the call (no parallel long-lived `inbound:` + dial alias both Connected for call-media)

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
- **`MeshHost` Amp underlay** ([A023](DECISIONS.md#a023--meshhost-may-own-ampstack-in-parallel-same-device-keys) / D10): `mesh_enabled` / `AttachAmpStack`; hard-require Amp; `Tick` pumps Amp
- **Product wiring:** `libp2p.mesh_enabled` (default **true**) → MessagingHub + pp-node; `TickLibp2p` calls `mesh_->Tick()` so Amp pumps
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

## Landed (D9 step 5a — Amp circuit/media-relay ownership)

- `MeshRuntime::AddIoTick` / `RemoveIoTick` — multiplex L4 deadline hooks on one Amp runtime (call-media + circuit + media-relay)
- `MeshHost` owns `CircuitTunnelCoordinator` + `AmpMediaRelayCoordinator` when Amp is up; `Start()` mirrors `host_circuit_relay` / `host_media_relay`
- `MessagingHub::ApplyMeshAdmissionPolicies` mirrors admission onto Amp coordinators
- **SoftMigrate stays on libp2p** (`MediaRelayService` / `CircuitRelayService`) — Amp media-relay still quote/attach-only (no Subscribe/SendFrame/local hop); Amp circuit returns `ChannelSession`, not a stream for SoftMigrate fan-out

## Landed (D9 step 5b — media-relay SoftMigrate single entry)

- **`AmpMediaRelayCoordinator` DATA plane** — Subscribe, SendFrame, AttachAsLocalHop, StartClientFrameReader, Detach, host fan-out on `ChannelSession`
- **`AmpMediaRelayClient`** — blocking `IMediaRelayClient` over coordinator ([A020])
- **`CallStack::WireMediaRelayDeps`** picks Amp when `MeshHost::AmpMediaRelayCoord()` is started; libp2p fallback unchanged
- **`PeerSessionDialRegistry`** mirrors ADP endpoints to Amp `PeerLinkManager` when on Amp path

## Landed (D9 step 5c — Amp circuit hop adopt for SoftMigrate NAT)

- **`AmpCircuitHopRegistry`** — `(peer × protocol) → ChannelSession` (libp2p `InstallCircuitHop` analogue)
- **`AmpCircuitHopReach`** — `ICircuitHopReach` via `CircuitTunnelCoordinator::StartBridge` + registry Install
- **`AmpMediaRelayCoordinator`** adopts circuit sessions for quote/attach (same pipe; do not close after quote)
- **MeshHost** always Starts Amp circuit + media-relay coordinators when Amp is up; inbound hosting gated by `SetServeInbound(host_*)`
- **DATA-plane hardening** — Fanout snapshots under lock (libp2p `MediaRelayRuntime` pattern); sync `Detach` / `AbortInflight` (no `PostIo(raw Impl*)` past Stop); protocol-keyed hop readiness (`Find(peer, protocol)`, not `HasAny` for call-media)
- **Call-media Amp over circuit** — design locked [A024](DECISIONS.md#a024--amp-call-media-over-circuit--nested-session) / [CALL_MEDIA_CIRCUIT.md](CALL_MEDIA_CIRCUIT.md); **implemented** (step 5d)

## Landed (D9 step 5d — Amp call-media nested Session over circuit / A024)

- **Carrier-neutral MSH** — `MshAdpHandshake` optional non-chunked wire; `PeerLink` carrier mode over bridged `ChannelSession`
- **`kAmpCircuitCarrierProtocolId`** — outer splice target (not product L4); `CircuitCarrierChannelPolicy` BestEffort + FRAG-friendly outbound queue
- **`PeerLinkManager::EstablishNestedOverCarrier` / `EnableNestedCarrierAccept`** — install virtual PeerLink after inner MSH; `OpenChannel` works without ADP endpoint; rekey refreshes protocol handlers
- **`AmpCircuitHopReach::TryEnsureCallMediaReachable`** — bridge carrier + nested Session (no `RegisterEndpoint`); media-relay path unchanged
- **`CallMediaLegCoordinator::StartLeg`** — reachable via endpoint **or** Connected nested/direct link
- **MeshHost** enables nested carrier accept whenever Amp L4 is up
- `pp_browser_p2p_test` — `AmpCircuitCallMediaComposeTest` (hello+audio, video >16KiB)

## Landed (D9 step 5e / 6 / 7 — blob + TCP underlay retire)

- **`AmpChatBlobService`** — `/pp-browser/chat-blob/1.0.0` single entry when Amp links present ([A020]); advertised on ch0
- **Amp UDP accept** always enabled
- **When Amp starts:** Amp L4 coords own dial-back + circuit/media-relay hosting (no TCP Identify/DialBack)
- **Deleted** transitional TCP-hello Opus dogfood: `CallMediaAdpDogfood.h`, `CallMediaAdpPath`, `CallMediaAdpKey`, hello `adp_*` fields
- Docs: NETWORKING / CALLS / LIBP2P_STREAMS / AMP-CHANNEL updated for Amp-primary mesh

## Plan adjustments (2026-08-31)

| Change | Why |
|--------|-----|
| **Do not block MeshHost Amp attach on AMP dial-back / mDNS** | Ownership is independent of reachability chrome; libp2p DialBack/Identify still cover probes until cutover |
| **Shared device ML-DSA keys required for `mesh_enabled`** | One PeerId across stacks ([A023](DECISIONS.md#a023--meshhost-may-own-ampstack-in-parallel-same-device-keys)) |
| **`MeshHost::Tick` must Pump Amp** | Idle UDP stack otherwise never completes MSH/ch0; product drives via MessagingHub Amp mesh pump (~5ms) + Connect/OpenChannel `io_pump` |
| **Amp start is soft-fail** | **Superseded by D10** — Amp hard-require; no TCP underlay fallback |
| **AMP dial-back is optional until Identify/TCP teardown** | **D10:** DialBack retired; Amp dial-back remains D8 follow-on |
| **Chat+history flip together** | Shared Amp address book / reachability; blob stays libp2p |
| **MeshRuntime IoTick must multiplex** | Multiple L4 coordinators share one Amp runtime; single `SetIoTick` overwrote peers |
| **Always Start Amp L4 coords; gate inbound with SetServeInbound** | SoftMigrate guests need outbound quote/attach/bridge without hosting |
| **Amp call-media circuit = nested Session (A024)** | Multi-channel A021 bundle cannot adopt a single opaque splice; single-pipe mode rejected |
| **Amp hop dialability is protocol-keyed** | `HasAny` must not short-circuit call-media when only media-relay hop exists |

## Landed (D10 — clean TCP mesh drop)

- **Hard-require Amp:** `MeshHost::Start` fails if `mesh_enabled` and Amp cannot bind/start (no soft-fail TCP underlay)
- **No silent Host:** when Amp succeeds, MeshHost does **not** construct `NodeRuntime` / `Libp2pHost` / `PeerSessionManager`
- **LAN keep:** PreferLocal / invite use `BuildAmpLanAdvertisedAddrs`; mDNS advertises with Amp UDP port + `amp_udp=` without TCP bound; SoftMigrate dial registry prefers Amp ADP MAs
- **Product L4:** chat / history / blob / call-media / SoftMigrate Amp-only; TCP DialBack / Identify / libp2p circuit+media-relay hosting not started
- **N025:** TCP ephemeral listen skipped when Amp is up (Amp UDP accept always on + mDNS)
- **pp-node / status:** Amp listen MA + Amp PeerId; dial-back probe deferred (reachability Unknown until D8)
- **Tests:** deleted TCP compose/direct/dial-back suites; Amp twins + logic tests retained

## Landed (D8 — Amp dial-back + reachability chrome)

- **`AmpDialBackService`** — same JSON probe as TCP DialBack over Amp ChannelSession; MeshHost owns + advertises `/pp-browser/dial-back/1.0.0`
- **`ReachabilityService`** restored — seed dial (ADP bootstrap) + dial-back + optional UPnP UDP; Me→Network / `pp-node --status` / startup probe
- **`BuildAmpReachabilityProbeTargets`** — public IPv4 + UPnP external as ADP MAs
- **Probes restored (Amp):** `pp-node-probe` (l1/fanout/cap/soak) + `pp-call-probe` (direct/hop/chat)
- **Note:** dial-back seed uses ADP bootstrap (`/udp/…/adp/1.0.0/p2p/…`); default Brief seed is UDP **443** (org must pin `amp_udp_port`)

## Landed (A017 — physical libp2p Host purge)

- **Deleted** `src/lib/libp2p` Host/TCP/Yamux/Noise/QUIC/protocol/muxer/security/network/connection/storage + `test/` + `example/`
- **Retained:** `p2p_peer_id`, `p2p_wire` (keys), multihash/multibase/SHA/log
- **Deps removed from tree:** `third_party/{lsquic,c-ares,tsl_hat_trie}` + CMake knobs `PP_BROWSER_LIBP2P_*`
- **Default Brief bootstrap** — ADP MA `/ip4/3.208.41.58/udp/443/adp/1.0.0/p2p/12D3KooW…`
- Product TCP L4 sources deleted earlier in A017 wave

## Landed (Track A — integration matrix + wire rekey)

- [TEST_MATRIX.md](TEST_MATRIX.md) — `A-INT-01` … `A-INT-09` mapped to STACK failure rows
- `amp_integration_harness.h` + `amp_integration_test.cpp` in `pp_browser_amp_link_test` (9 Tier-B tests)
- `SessionControl` codec (wire v2 on ch0) + `Session::ApplyRekey` grace window (1000 ms)
- `PeerLink::RequestSessionRekey()` — coordinated rekey over ch0 after capability exchange
- `pp_browser_amp_session_test` — `SessionControlCodecTest`, `SessionRekeyGraceTest`

## Landed (Track A-adv — adversarial hardening)

- `PeerLinkConfig::dial_timeout` enforced in `PeerLinkManager::Tick` (Handshaking/Dialing)
- `PeerLinkConfig::max_links` enforced on outbound dial + inbound accept
- `A-ADV-02` … `A-ADV-08` integration tests (see [TEST_MATRIX.md](TEST_MATRIX.md))
- `ChannelMux::InjectSealedForTest` — harness hook for sealed FRAG injection

## Next (implementation)

1. **Listen policy polish** / mDNS-only edge cases (D8 follow-on)
2. **Org seed deploy** — ship `pp-node` with `PP_NODE_AMP_UDP_PORT=443` so default ADP bootstrap dials succeed
3. **A027 follow-on** — migrate L4 TearDown-from-`on_frame_` to signal → parent drop after dispatch (call-media / media-relay / circuit); see repo [OWNERSHIP.md](../../docs/architecture/OWNERSHIP.md)
4. **A-INT-10** (stretch) — MSH chunk loss + ADP rtx during handshake

### D9 cutover checklist

Do **not** dual `if (amp)` in product paths ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol)). Flip one protocol family at a time behind MeshHost composition, then delete the libp2p path.

| Step | Action | Notes |
|------|--------|--------|
| 1–7 | D9 Amp ownership + TCP listen retire | **Done** |
| D10 | Hard-require Amp + drop silent Host + LAN Amp advertise | **Done** |
| D8 | Amp dial-back + UPnP UDP + probes | **Done** |
| A017 | Delete idle TCP L4 + purge libp2p Host tree | **Done** |

See [PHASES.md](PHASES.md) for full ordering.
