# Amp call-media over circuit

**Status:** Implemented ([A024](DECISIONS.md#a024--amp-call-media-over-circuit--nested-session)); golden tests green  
**Depends on:** D7a circuit tunnel, D9 step 5c hop registry, [A019](DECISIONS.md#a019--circuit-relay--channel-tunnel), [A021](DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime)  
**Unblocks:** D9 step 6 (retire Identify / TCP mesh listen) for NAT 1:1 calls

## Problem

Libp2p call-media is **one ordered stream**: hello → encrypted media on the same pipe. Circuit installs that stream under `(peer × call-media protocol)`; `OpenStream` returns it.

Amp call-media ([A021](DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime)) is a **control + media channel bundle** on a direct `PeerLink`:

| Role | Channel class | Why |
|------|---------------|-----|
| Control | Reliable `RealtimeControl` | Hello / glare / teardown |
| Media | BestEffort `Realtime` | Opus / video; sent as it comes |

D9 step 5c installed Amp **media-relay** over circuit by adopting one opaque `ChannelSession`. That channel was reliable and carried the media too (no data lane); since pp-cpp-mesh v7.0.0 a hop behind a circuit is reached by the same nested Session as a call's peer, and its control channel and data lane open on it ([V065](../p2p-av-calls/DECISIONS.md#v065--call-audio-keeps-a-delay-budget-audio-before-video) 6).

## Decision: nested end-to-end Session (A024)

Treat the circuit `ChannelSession` as a **carrier** for an authenticated A↔B MSH, then run an inner `Session` + `ChannelMux` (virtual peer link). `CallMediaLegCoordinator` keeps its normal bundle semantics on that virtual link.

```
A ──MSH── R ──MSH── B     (outer associations)
A ══MSH════════════════ B  (inner Session over spliced carrier; A024)
     └─ ChannelMux ─┬─ control (Reliable)
                    └─ media   (BestEffort)
```

### Landed shape

| Piece | Location |
|-------|----------|
| Outer protocol | `kAmpCircuitCarrierProtocolId` (`/pp-amp/circuit-carrier/1.0.0`) |
| Outer policy | `CircuitCarrierChannelPolicy` — BestEffort (avoids ADP reliable_window stall) |
| Carrier MSH | `PeerLink` carrier ctor + `MshAdpHandshake(chunked_wire=false)` |
| Install / accept | `PeerLinkManager::EstablishNestedOverCarrier` / `EnableNestedCarrierAccept` |
| Reach | `AmpCircuitHopReach::TryEnsurePeerReachable`, and `TryEnsureHopReachable` for a media-relay hop (no `RegisterEndpoint`) |
| L4 | `CallMediaLegCoordinator` opens via Connected nested link without ADP endpoint |
| Tests | `amp_circuit_call_media_compose_test.cpp` |

### QoS on the carrier

v1 uses a **BestEffort** outer splice so large media FRAG bursts are not capped by ADP `reliable_window`. Inner mux still uses Reliable control + BestEffort media. Since pp-cpp-amp v2.5.0 the nested link retransmits its Reliable-class frames end to end (`CarrierLane`, ADR_LINK_PLANE §11) instead of dual outer lanes — relays unchanged.

## SoftMigrate / D9 step 6 dependency

| Path | Amp circuit status |
|------|--------------------|
| SoftMigrate media-relay NAT | **Done** (5c) — single-channel adopt |
| 1:1 call-media NAT | **Done** (5d) — nested Session |
| Step 6 Identify/TCP teardown | **Done** when Amp starts (blob Amp + Amp accept) |
