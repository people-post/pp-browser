# Amp call-media over circuit

**Status:** Design locked ([A024](DECISIONS.md#a024--amp-call-media-over-circuit--nested-session)); implementation not started  
**Depends on:** D7a circuit tunnel, D9 step 5c hop registry, [A019](DECISIONS.md#a019--circuit-relay--channel-tunnel), [A021](DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime)  
**Blocks:** D9 step 6 (retire Identify / TCP mesh listen) for NAT 1:1 calls

## Problem

Libp2p call-media is **one ordered stream**: hello → encrypted media on the same pipe. Circuit installs that stream under `(peer × call-media protocol)`; `OpenStream` returns it.

Amp call-media ([A021](DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime)) is a **control + media channel bundle** on a direct `PeerLink`:

| Role | Channel class | Why |
|------|---------------|-----|
| Control | Reliable `RealtimeControl` | Hello / glare / teardown |
| Media | BestEffort `Realtime` | Opus / video; drop-oldest |

D9 step 5c installed Amp **media-relay** over circuit by adopting one opaque `ChannelSession` — correct because SoftMigrate’s client leg is already a single long-lived channel.

`CallMediaLegCoordinator` never consults `AmpCircuitHopRegistry`. `AmpCircuitHopReach::TryEnsureCallMediaReachable` can install a pipe, but nothing opens the A021 bundle through it.

## Why a single opaque splice is wrong for Amp call-media

1. **Fixed QoS** — circuit far channel is Reliable `Control`; media needs BestEffort.
2. **Single handler / lifecycle** — one `ChannelSession` cannot host independent control and media state.
3. **Wrong peer identity** — B’s R↔B link authenticates R, not A; glare and PeerId ordering break.
4. **No sibling OPEN** — opening a second channel on the outer link opens toward R, not through the tunnel.
5. **A019** — full blindness wants end-to-end A↔B Session; today’s tunnel forwards L4 bodies after relay-terminated handshakes.

A temporary “single-pipe call-media mode” (tag hello + media on one channel) would fight A021, reintroduce head-of-line blocking or unreliable control, and still need origin authentication. **Rejected** for product path.

Two parallel circuit tunnels (control + media) preserve QoS but keep relay-terminated identity and protocol-specific pairing. **Rejected** as the long-term shape; may inform interim experiments only if nested Session slips.

## Decision: nested end-to-end Session (A024)

Treat the circuit `ChannelSession` as a **carrier** for an authenticated A↔B MSH, then run an inner `Session` + `ChannelMux` (virtual peer link). `CallMediaLegCoordinator` keeps its normal bundle semantics on that virtual link.

```
A ──MSH── R ──MSH── B     (outer associations; today)
A ══MSH════════════════ B  (inner Session over spliced carrier; A024)
     └─ ChannelMux ─┬─ control (Reliable)
                    └─ media   (BestEffort)
```

Relay forwards **opaque carrier bytes** (preferably inner L2 ciphertext per A019). Product L4 never sees the splice.

### QoS on the carrier

A single Reliable outer channel flattens BestEffort media. Acceptable **v1** if the outer pipe is BestEffort and control uses inner reliability — **or** the tunnel grows separate reliable / best-effort lanes. Spec the carriage in `AMP-CHANNEL.md` when implementing; do not silently retransmit media on a Reliable-only splice.

### Registry shape

`AmpCircuitHopRegistry` today stores one protocol-bound `ChannelSession`. Nested Session needs either:

- a **virtual link** object (inner Session + Mux + remote PeerId + carrier ownership), keyed by peer (protocol opens go through the Mux), or
- hop table entry upgraded from “raw session” → “circuit-backed PeerLink”.

`PeerLinkManager::OpenChannel` should resolve direct **or** circuit-backed routes through one interface so L4 stays A020-clean.

## Implementation sketch (ordered)

1. **Carrier-neutral MSH** — extract drive loop from `MshAdpHandshake` so a `ChannelSession` (or byte pipe) can complete MSH without ADP datagrams.
2. **`CircuitPeerLink` (name TBD)** — owns carrier session, inner Session/Mux, authenticated remote PeerId, close propagation.
3. **Registry / PeerLinkManager** — Install virtual link after bridge + inner MSH; `OpenChannel(peer, protocol)` works for call-media without L4 knowing about circuits.
4. **`CallMediaLegCoordinator`** — drop “direct endpoint required”; use the unified open path (direct or circuit).
5. **`AmpCircuitHopReach`** — establish/find virtual link; never treat `HasAny(peer)` as call-media readiness (protocol- / link-specific).
6. **Golden test** — `amp_circuit_call_media_compose_test` (hello, glare, audio, large video, close); SoftMigrate coexistence with media-relay hop on same mesh.
7. **Docs** — amend A019/A022 notes; update `AMP-CHANNEL.md` nested-session wire + QoS carriage.

## Explicit non-goals (this design)

- Dual `if (amp)` in SoftMigrate / CallLibp2pMediaBridge ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol)).
- Retiring Identify/TCP (D9 step 6) before Amp 1:1 circuit call-media works for peers without direct ADP.
- Changing A021 bundle shape to match libp2p’s single stream.

## SoftMigrate / D9 step 6 dependency

| Path | Amp circuit status |
|------|--------------------|
| SoftMigrate media-relay NAT | **Done** (5c) — single-channel adopt |
| 1:1 call-media NAT | **Blocked** on this design |
| Step 6 Identify/TCP teardown | Needs call-media circuit + Amp-native listen/dial book |

Until nested Session lands, PreferLocal / SoftMigrate group media can use Amp; **direct 1:1 over NAT still needs the libp2p call-media circuit path** (or direct ADP).
