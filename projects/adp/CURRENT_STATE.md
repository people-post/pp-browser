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

## Next (implementation)

1. **D7b** — `AmpMediaRelayCoordinator` + `MediaRelayBundleLogic` (A022 template)
2. **D8** — reachability / ch0 caps on ADP multiaddrs
3. **D9** — single cutover: wire AMP L4 into `MeshHost` / `CallStack`; retire `CallMediaAdpDogfood` / TCP-hello path

See [PHASES.md](PHASES.md) for full ordering.
