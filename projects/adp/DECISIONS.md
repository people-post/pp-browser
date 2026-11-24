# Decisions — ADP

The library's decisions — how the transport itself works (A001–A005, A007, A012–A016, A018, A019, A024–A027, A029) — moved to pp-cpp-amp [docs/DECISIONS.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md) with their ids ([CR026](../capability-repos/DECISIONS.md#cr026--pp-cpp-amp-takes-the-stacks-design-and-the-contracts-it-alone-implements)). Each keeps its heading here, pointing at its new place, so old links resolve. The app's decisions about its use of the stack stay here in full: A006, A008–A011, A017, A020–A023, A028.

## A001 — Name ADP + folder/target names

Moved to pp-cpp-amp: [docs/DECISIONS.md § A001](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a001--name-adp--foldertarget-names)

## A002 — L1 HMAC binding vs L2 crypto

Moved to pp-cpp-amp: [docs/DECISIONS.md § A002](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a002--l1-hmac-binding-vs-l2-crypto)

## A003 — Message-oriented connection + path migrate

Moved to pp-cpp-amp: [docs/DECISIONS.md § A003](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a003--message-oriented-connection--path-migrate)

## A004 — Wire v1 + HMAC-SHA256-128 + LE + max 1200

Moved to pp-cpp-amp: [docs/DECISIONS.md § A004](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a004--wire-v1--hmac-sha256-128--le--max-1200)

## A005 — Reliable = ACK + rtx; BestEffort = no rtx

Moved to pp-cpp-amp: [docs/DECISIONS.md § A005](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a005--reliable--ack--rtx-besteffort--no-rtx)

## A006 — Browser-first; extract shared lib later

**Date:** 2026-08-30  
**Decision:** Implement in pp-browser `lib/amp/L1` only. No pp-cpp-common / pp-ledger consumer in foundation.  
**Rationale:** Dogfood path migration + lossy/reliable where it hurts.  
**Alternatives:** Start in common or ledger.

## A007 — Asio-free DatagramIo

Moved to pp-cpp-amp: [docs/DECISIONS.md § A007](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a007--asio-free-datagramio)

## A008 — First consumer = Opus BestEffort

**Date:** 2026-08-30  
**Decision:** First product dogfood of ADP is **1:1 call Opus (channel 0) BestEffort** only. Video, signaling, hello/key exchange, SoftMigrate/`media_relay` stay on TCP+Noise+Yamux.  
**Rationale:** Matches phone IP-churn motivation; ADP v1 max payload 1200 B fits Opus, not typical H264 AUs.  
**Alternatives:** Control RPC first; video-over-ADP with fragmentation.

## A009 — `K_assoc` from call media key

**Date:** 2026-08-30  
**Decision:** Derive 32-byte `K_assoc` via HKDF-SHA256 from the existing call media key. Info string `pp-adp-call-media-v1|call_id:<id>|epoch:<n>`. Never send `K_assoc` on the wire.  
**Rationale:** Reuses call key agreement on TCP hello; ADP stays L1 binding only.  
**Alternatives:** Separate key exchange; send key in hello.

## A010 — Hello extension (TCP) + local IP

**Date:** 2026-08-30  
**Decision:** Additive JSON on call-media `hello` / `hello_ack` (ignore if absent): `adp_v` (1), `adp_port`, `adp_assoc` (32 hex chars), `adp_ip` (dotted IPv4 for outbound dial). Offerer mints `adp_assoc`; answerer echoes it.  
**Rationale:** Need peer IP+port before first Opus packet; TCP hello already runs.  
**Alternatives:** Learn IP only from first packet; use libp2p remote multiaddr only.

## A011 — Fallback to TCP stream Opus

**Date:** 2026-08-30  
**Decision:** If peer omits ADP fields, TEMP dogfood gate `kCallMediaAdpOpusDogfood` is false, bind fails, or path is not alive after grace, Opus uses existing `SendMedia` stream path. Calls must not fail solely because ADP is unavailable. Gate lives in `CallMediaAdpDogfood.h` (not settings) and is deleted when Opus-over-ADP becomes default-on.  
**Rationale:** Safe dogfood; dual-NAT without punch falls back cleanly; no config surface.  
**Alternatives:** Hard-require ADP; persist `libp2p.adp_opus` in config.json (rejected for short dogfood).  
**Status:** **Transitional.** Superseded for product direction by [A012](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a012--amp-native-stack-option-b). Delete with Phase D6.

## A012 — AMP-native stack (Option B)

Moved to pp-cpp-amp: [docs/DECISIONS.md § A012](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a012--amp-native-stack-option-b)

## A013 — L2-full Session only

Moved to pp-cpp-amp: [docs/DECISIONS.md § A013](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a013--l2-full-session-only)

## A014 — One Association per peer pair

Moved to pp-cpp-amp: [docs/DECISIONS.md § A014](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a014--one-association-per-peer-pair)

## A015 — `K_assoc` and `K_session` from MSH transcript

Moved to pp-cpp-amp: [docs/DECISIONS.md § A015](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a015--k_assoc-and-k_session-from-msh-transcript)

## A016 — Channel 0 = capability / identify plane

Moved to pp-cpp-amp: [docs/DECISIONS.md § A016](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a016--channel-0--capability--identify-plane)

## A017 — libp2p shrink: retain crypto + PeerId only

**Date:** 2026-08-30  
**Decision:** Vendored libp2p **retires** from product path: TCP/QUIC transport, Yamux, Noise wire, `Host`, `newStream`, multistream-select. **Retain** ML-DSA PeerId, multihash, crypto providers (or migrate to `pp-cpp-crypto` later). `base/mesh` must not link libp2p. Amend N022 spirit: mesh semantics continue; libp2p is not the underlay.  
**Rationale:** Clear ownership; AMP owns dial/mux/crypto on UDP.  
**Alternatives:** Keep libp2p Host with ADP transport (Option C).

## A018 — Fragmentation at L3, not L1

Moved to pp-cpp-amp: [docs/DECISIONS.md § A018](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a018--fragmentation-at-l3-not-l1)

## A019 — Circuit relay = channel tunnel

Moved to pp-cpp-amp: [docs/DECISIONS.md § A019](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a019--circuit-relay--channel-tunnel)

## A020 — Single transport entry per protocol

**Date:** 2026-08-30  
**Decision:** Each L4 protocol uses one **`ChannelSession`** entry point. No scattered `if (amp) … else (libp2p)` in feature code during migration — parallel stacks only in tests until cutover.  
**Rationale:** Prevents dual-stack ossification.  
**Alternatives:** Per-protocol feature flags with dual paths in production.

## A021 — Call-media = channel bundle on MeshRuntime

**Date:** 2026-08-31  
**Decision:** AMP call-media is a **`call_id`-keyed control + media channel bundle** on `MeshRuntime`, not a blocking libp2p-style stream adapter. Bundle phase is AMP-native (`OutboundHello` / `InboundHello` / `AwaitingMedia` / `MediaReady`); glare and channel-close admit rules live in pure `CallMediaBundleLogic`. Channels are role-tagged (outbound control / inbound control / media); provisional closes use `CloseQuiet` / slot-first teardown so abandoned roles cannot fail the winning bundle. L4 is non-blocking (`StartLeg` + completion callback); inbound hello may run on a worker lane with wire ops posted back to the io thread. Dual-dial glare: **higher base58 PeerId** keeps outbound; lower abandons outbound and adopts inbound.  
**Rationale:** Removes IoPump blocking, shared “active control” pointer races, and stream-era phase tables from the AMP path; matches libp2p golden behavior without API parity.  
**Alternatives:** Port `CallMediaDirectService::Connect` blocking semantics to AMP; single combined control+media channel; keep `CallMediaSessionPhase` as the AMP source of truth.

## A022 — Circuit tunnel = non-blocking coordinator on MeshRuntime

**Date:** 2026-08-31  
**Decision:** AMP circuit relay is a **`CircuitTunnelId`-keyed tunnel** on `MeshRuntime`, not a blocking `RequestBridge` + `StreamBridge` port. Public API is non-blocking (`StartBridge` + completion callback). Phase machine and admission live in pure `CircuitBundleLogic`. Runtime (`CircuitTunnelCoordinator`) drives `EnsureAssociation` / `OpenChannel` via **callbacks + `PostToIo` only** — L4 must never call `MeshRuntime::Pump()` / `IoPumpUntil`. `ChannelBridge` remains an io-thread DATA splice helper armed **after** handshake, never via `SetFrameHandler` from inside an active frame handler. D7b media-relay follows this same template.  
**Rationale:** Same failure modes A021 fixed — nested pump reentrancy, handler UAF from mid-callback `SetFrameHandler`, mutex/Stop deadlock, Browser-IO stalls on sync waits. Libp2p circuit blocks on a **worker** lane; AMP put the same waits on the pump thread in transitional D7a.  
**Alternatives:** Keep sync `AmpCircuitRelayService::RequestBridge` with more `PostToIo` workarounds; port `CircuitRelayRuntime::WaitReadyOrStop` onto MeshRuntime.

## A023 — MeshHost may own AmpStack in parallel; same device keys

**Date:** 2026-08-31  
**Decision:** `MeshHost` may construct / attach an `pp::amp::AmpStack` **beside** libp2p without routing product L4 ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol) still forbids dual `if (amp)` in app paths). `enable_amp_stack` **requires** the same `device_ml_dsa_*` keys as `Libp2pHost` so AMP PeerId matches. Amp start failure is **soft** (libp2p stays up; `AmpLastError()`). `MeshHost::Tick` pumps the Amp stack — product must call `mesh->Tick()` (not only `Runtime()->Tick()`). Dial-back / mDNS on ADP are **not** blockers for this ownership step.  
**Rationale:** Cutover needs a live UDP+MSH host object before flipping protocols; separate keys would fork identity; hard-failing Amp would take down production mesh during the parallel phase.  
**Alternatives:** Separate Amp-only process; generate a second PeerId for AMP; hard-fail MeshHost::Start when Amp fails.

## A024 — Amp call-media over circuit = nested Session

Moved to pp-cpp-amp: [docs/DECISIONS.md § A024](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a024--amp-call-media-over-circuit--nested-session)

## A025 — Pre-extract layer cleanup (limits, policies, PeerId)

Moved to pp-cpp-amp: [docs/DECISIONS.md § A025](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a025--pre-extract-layer-cleanup-limits-policies-peerid)

## A026 — One Session per PeerId under dual-dial (mesh election)

Moved to pp-cpp-amp: [docs/DECISIONS.md § A026](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a026--one-session-per-peerid-under-dual-dial-mesh-election)

## A027 — Parent-only destroy (L3/L4 ownership hierarchy)

Moved to pp-cpp-amp: [docs/DECISIONS.md § A027](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy)

## A028 — L4 protocol kinds = seven conversation shapes

**Date:** 2026-09-04  
**Status:** Accepted (design)  
**Normative:** [L4_PROTOCOL_KINDS.md](../../docs/contracts/L4_PROTOCOL_KINDS.md)  
**Amends / cross-links:** [A016](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a016--channel-0--capability--identify-plane), [A019](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a019--circuit-relay--channel-tunnel), [A020](#a020--single-transport-entry-per-protocol), [A024](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a024--amp-call-media-over-circuit--nested-session); mesh [N021](../p2p-mesh/DECISIONS.md#n021--generic-media_relay-framing-qos-channel-types), [N017](../p2p-mesh/DECISIONS.md#n017--split-n4-media-sfu-first-message-relay-separate-pricing-later), [N030](../p2p-mesh/DECISIONS.md#n030--adopt-l4-protocol-kinds-gate).

**Decision:** Freeze L4 taxonomy as **seven conversation kinds** — **identify**, **discover**, **reach**, **circuit**, **rpc**, **blob**, **realtime** — not one `protocol_id` per product feature. AMP L1–L3 already own mux/QoS/crypto; a new L4 id is allowed only when the **conversation contract** is new (QoS/duplex/lifetime, security posture, or failure model). Prefer envelope `op`, `channel_type` / frame fields, circuit `target_protocol`, and PeerLink policy for feature growth. Current shipped ids map onto these kinds; merges/renames are optional and versioned — do not big-bang rewrite wire strings. Carrier (`amp-circuit-carrier`) stays plumbing, not a product kind. Rejected as new kinds: always-on `call-signal`; A/V-specific relay forks; per-feature tunnels; treating deferred `message_relay` as a distinct wire shape (it is an **rpc** host role).

**Rationale:** Stops protocol sprawl while covering find → meet → talk → bulk → live (E2E or blind hop). Aligns existing anti-proliferation ADRs (ch0, generic circuit, generic media hop, V033 RPC vs SM) into one agent-facing gate.

**Converted (2026-09-04):** Product not released — wire ids renamed in-place to kind names (`/pp-browser/rpc|blob|realtime|datagram-relay|circuit|reach/1.0.0`, punch `/pp-browser/reach/punch/1.0.0`). `rpc` kind uses separate OPEN ids `/pp-browser/rpc/chat/1.0.0` and `/pp-browser/rpc/history/1.0.0` (handler ownership; no first-frame demux); punch keeps a distinct reach-kind id (multi-frame SM). Nested-Session carrier uses Amp default `/amp/circuit-carrier/1.0.0` (no product `/pp-browser/circuit-carrier` id).

**Prepared compositions (2026-09-05):** IPFS-*like* content share = **discover** + **blob** (+ **rpc** announce/meta); live broadcast = **rpc** control + **realtime** blind hop (optional **blob** DVR). Documented in [L4_PROTOCOL_KINDS § Prepared compositions](../../docs/contracts/L4_PROTOCOL_KINDS.md#prepared-compositions-no-new-kinds) — no new kinds.

**Alternatives rejected:** One mega “mesh-control” channel for discover+reach+dht (already considered and rejected for DHT in N028); feature-named protocols forever; collapsing E2E call-media and blind media-relay into a single id (different security postures under the **realtime** kind).

## A029 — A smaller packet: one sealed header, derived nonces, no second tag

Moved to pp-cpp-amp: [docs/DECISIONS.md § A029](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag)
