# Phases — ADP / AMP stack

Foundation L1 (Phases 0–3) is **done**. Phase 4 (Opus TCP side-path) is **transitional** — superseded by AMP migration ([STACK.md](STACK.md), [A012](DECISIONS.md#a012--amp-native-stack-option-b)).

## Phase 0 — Project + ADRs (L1)

- [x] README / DESIGN / DECISIONS / PHASES / CURRENT_STATE
- [x] ADRs A001–A007
- [x] Register in `projects/README.md` + `SRC_LAYOUT.md`

## Phase 1 — Codec + HMAC + DatagramIo + best-effort

- [x] `WireCodec`, `HmacBinder`, constants
- [x] `DatagramIo`, `MemoryDatagramIo`, `OsUdpDatagramIo`
- [x] `Endpoint` demux + `Connection` best-effort + path migrate + close
- [x] Tests: wire / HMAC / replay / skew / lifecycle / path / demux / NAT / best-effort

## Phase 2 — Reliable delivery

- [x] ACK + rtx + send window + virtual clock
- [x] Tests: reliable / ack / qos isolation / shutdown under loss

## Phase 3 — Harden (L1)

- [x] Packet mutilator, OsUdp loopback smoke, multi-connection stress
- [x] Promote `docs/contracts/ADP.md`
- [x] `pp_browser_adp_test` green

## Phase 4 — Opus side-path (transitional; legacy TCP bootstrap)

- [x] ADRs A008–A011
- [x] `CallMediaAdpPath` + HKDF + hello negotiate + bridge Opus path
- [x] TEMP dogfood gate `CallMediaAdpDogfood.h`
- [ ] **Superseded:** delete when D6 call-media on AMP ships ([A015](DECISIONS.md#a015--k_assoc-and-k_session-from-msh-transcript))

---

## AMP migration (D0–D9)

Coding-first; `MemoryDatagramIo` + unit tests before integration.

### D0 — Stack constitution

- [x] [STACK.md](STACK.md)
- [x] [AMP-SESSION.md](../../docs/contracts/AMP-SESSION.md)
- [x] [AMP-CHANNEL.md](../../docs/contracts/AMP-CHANNEL.md)
- [x] ADRs A012–A020
- [x] Register contracts in `docs/README.md`
- [x] `SRC_LAYOUT.md` — planned `base/mesh/`

### D1 — L2 Session (MSH + full AEAD)

- [x] `src/base/mesh/session/` + `pp_base_mesh_session`
- [x] MSH v1 state machine + KDF (`K_assoc`, `K_session`)
- [x] Seal/open with AAD (`session_epoch`, `channel_id`, `channel_seq`)
- [x] Rekey (epoch bump; `k_assoc` stable, send/recv rotate)
- [x] `pp_browser_amp_session_test` (KAT + 2-peer handshake)

### D2 — L3 Channel mux

- [x] `src/base/mesh/channel/` + `pp_base_mesh_channel`
- [x] OPEN/ACK/DATA/CLOSE/RESET
- [x] Channel 0 capability plane
- [x] QoS map (channel class → ADP Reliable/BE)
- [x] `ChannelSession` (replaces `DuplexFrameSession` API shape)
- [x] `pp_browser_amp_channel_test` (14 tests, green)

### D3 — L3 fragmentation

- [x] Reliable FRAG reassembly (256 KiB control max)
- [x] Loss / reorder / dup / timeout tests (`message_reassembly_test.cpp`)

### D4 — PeerLinkManager

- [x] `src/base/mesh/link/` + `pp_base_mesh_link`
- [x] `EnsureAssociation` + `OpenChannel` API
- [x] Multiaddr `/ip4/.../udp/.../adp/1.0.0/p2p/...`
- [x] Warm/cold, backoff (ported from `PeerSessionManager` shape)
- [x] `MeshPump` integration on io thread
- [x] MSH-over-ADP with chunking for payloads > ADP MTU
- [x] `pp_browser_amp_link_test` (3 tests, green)

### D5 — L4 port: chat + history

- [x] `/pp-browser/chat/1.0.0` on ChannelSession
- [x] `/pp-browser/chat-history/1.0.0`
- [x] Port gtests; single transport entry ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol))

### D6 — L4 port: call-media

- [x] `CallMediaLegCoordinator` — non-blocking hello on Reliable `RealtimeControl` + AEAD on BestEffort `Realtime` (`MeshRuntime` + `PeerLinkManager::OpenChannel`)
- [x] `MeshRuntime` — io-thread `Pump`/`PostToIo` harness for AMP L4 tests
- [x] L3 remote terminal events (`peer_close` / `peer_reset`) propagate to `ChannelSession`
- [x] PeerId-keyed link merge (`FindLinkByPeerId`, inbound adopt/rekey)
- [x] `CallMediaControlChannelPolicy` + split control/media channel bundle (normative AMP-CHANNEL)
- [x] `CallMediaLegCoordinatorTest` — 8 tests + `CallMediaLegTripleTest` (audio, video, detach, timeout, handler clear, k-cycle, dual-dial glare, conflict)
- [x] Shared `mesh_test_harness.h` + `MeshRuntime` for AMP L4 tests
- [x] Wire `CallMediaLegCoordinator` into `CallStack` via `CallMediaAmpTransport` ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol))
- [x] Disable `kCallMediaAdpOpusDogfood` (legacy side-path idle; delete header in D9 step 7)
- [x] Delete `CallMediaAdpDogfood.h` + TCP-hello `K_assoc` ([A015](DECISIONS.md#a015--k_assoc-and-k_session-from-msh-transcript))
- [x] SoftMigrate media-relay single entry via `AmpMediaRelayClient` when Amp is up ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol))
- [x] Amp circuit hop registry + SoftMigrate NAT reach (`AmpCircuitHopReach` / media-relay adopt)
- [x] Amp call-media circuit — nested Session ([A024](DECISIONS.md#a024--amp-call-media-over-circuit--nested-session), [CALL_MEDIA_CIRCUIT.md](CALL_MEDIA_CIRCUIT.md))

### D7 — L4 port: circuit + media-relay

- [x] **D7a** Channel tunnel — `ChannelBridge` splice + **`CircuitTunnelCoordinator`** / `CircuitBundleLogic` on `MeshRuntime` ([A022](DECISIONS.md#a022--circuit-tunnel--non-blocking-coordinator-on-meshruntime)); non-blocking `StartBridge` (no `IoPumpUntil`)
- [x] `CircuitTunnelChannelPolicy` (+ media-relay policies stubbed for D7b)
- [x] `pp_browser_p2p_test` — `CircuitBundleLogicTest` + `CircuitTunnelCoordinatorTest`
- [x] **D7b** `AmpMediaRelayCoordinator` + `MediaRelayBundleLogic` — quote/accept/attach on ChannelSession (A022; fan-out SoftMigrate deferred)
- [x] Nested A↔B Session through tunnel (full [A019](DECISIONS.md#a019--circuit-relay--channel-tunnel) blind L2) — [A024](DECISIONS.md#a024--amp-call-media-over-circuit--nested-session) / [CALL_MEDIA_CIRCUIT.md](CALL_MEDIA_CIRCUIT.md)

### D8 — Reachability

- [x] ch0 capability exchange on `PeerLinkManager` after MSH ([A016](DECISIONS.md#a016--channel-0--capability--identify-plane))
- [x] Ingest remote ADP listen addrs under authenticated PeerId (`PreferredMultiaddr` / peer-id dial)
- [x] AMP dial-back protocol (`AmpDialBackService` / `/pp-browser/dial-back/1.0.0`) + Me→Network / pp-node probe
- [x] UPnP **UDP** mapping attempt during reachability probe (`TryUpnpUdpPortMapping`)
- [ ] Listen policy polish / mDNS-only edge cases (follow-on)

### D9 — Retire legacy underlay

- [x] `AmpStack` composition helper (`Endpoint` + `MeshRuntime`)
- [x] Attach `AmpStack` to `MeshHost` (parallel; `enable_amp_stack` / `AttachAmpStack`; [A023](DECISIONS.md#a023--meshhost-may-own-ampstack-in-parallel-same-device-keys))
- [x] Enable Amp in product node/MessagingHub start (default on; soft-fail)
- [x] Chat + history single transport entry via Amp when `MeshHost::Amp()` is up ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol))
- [x] Call-media single transport entry via `CallMediaAmpTransport` / `CallMediaLegCoordinator` when Amp is up; `kCallMediaAdpOpusDogfood=false`
- [x] Circuit / media-relay Amp ownership on MeshHost (step 5a) — hosting + admission
- [x] SoftMigrate media-relay single entry (step 5b)
- [x] Amp circuit adopt for SoftMigrate NAT (step 5c) — call-media = A024
- [x] Amp call-media nested Session over circuit (step 5d / A024)
- [x] Amp chat-blob single entry (step 5e)
- [x] Stop Identify / TCP mesh listen when Amp owns mesh (step 6)
- [x] Delete dogfood + TCP-hello Opus; update NETWORKING / LIBP2P_STREAMS / CALLS (step 7)
- [x] Amp dial-back (D8) for reachability chrome

### D10 — Clean TCP mesh drop

- [x] Hard-require Amp (`enable_amp_stack`): Amp bind/start failure fails mesh start (no TCP underlay fallback)
- [x] PreferLocal / invite + LAN mDNS work Amp-only (`BuildAmpLanAdvertisedAddrs`, mDNS `amp_udp` without TCP bind)
- [x] Product composition Amp-only (chat/history/blob/call-media/SoftMigrate); collapse libp2p L4 branches
- [x] No product `Libp2pHost` / `PeerSessionManager` when Amp owns mesh; N025 TCP ephemeral retired (Amp accept + mDNS)
- [x] Delete TCP-only compose/unit tests (Amp twins retained); DialBack/Identify chrome deferred
- [x] **A017 wave:** delete idle TCP L4 sources; MeshHost Amp-only API; ADP Brief bootstrap; libp2p Host tree CMake-gated (default OFF)
- [ ] Physical delete unused `src/lib/libp2p` Host/TCP/Yamux/Noise sources ([A017](DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only))

---

## Run tests

```bash
cmake -S . -B build -DPP_BROWSER_BUILD_TESTS=ON
cmake --build build --target pp_browser_adp_test pp_browser_amp_session_test pp_browser_amp_channel_test pp_browser_amp_link_test pp_browser_p2p_test -j
./build/src/base/adp/tests/pp_browser_adp_test
./build/src/base/mesh/session/tests/pp_browser_amp_session_test
./build/src/base/mesh/channel/tests/pp_browser_amp_channel_test
./build/src/base/mesh/link/tests/pp_browser_amp_link_test
```
