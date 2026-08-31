# AMP Channel — L3 normative contract

**Status:** Foundation spec (2026-08-30). Normative for `base/mesh/channel` (planned).  
**Stack:** [STACK.md](../../projects/adp/STACK.md) · L2 [AMP-SESSION.md](AMP-SESSION.md) · L1 [ADP.md](ADP.md)  
**Version axes:** `channel_frame_version`, per-channel `protocol_id`

## Role

L3 **AMP Channel** replaces Yamux + app `StreamFrameIo` framing:

- Multiplex logical conversations on one **Session**
- Map channel class → ADP QoS ([STACK.md § QoS](../../projects/adp/STACK.md#qos-mapping-fixed-at-channel-open))
- Fragment/reassemble large L4 messages (up to app max, e.g. 256 KiB)
- Port `StreamIoPolicy` semantics (drop oldest, read_once, timeouts)

L2 AEAD wraps **L3 frame bytes**. L4 payloads inside L3 DATA frames are unchanged ([WIRE_SCHEMAS.md](WIRE_SCHEMAS.md)).

## Channel identifiers

| Id | Reserved use |
|----|----------------|
| **0** | Capability / identify plane ([A016](../../projects/adp/DECISIONS.md#a016--channel-0--capability--identify-plane)) |
| **1 … 0xFFFF_FFFE** | Dynamic; allocated by opener |
| **0xFFFF_FFFF** | Illegal |

`protocol_id` is a UTF-8 string (e.g. `/pp-browser/chat/1.0.0`) carried in OPEN.

## Channel classes

| Class | ADP QoS | Duplex | Typical `protocol_id` |
|-------|---------|--------|------------------------|
| **Transactional** | Reliable | half; `read_once` | `/pp-browser/chat/1.0.0`, `/pp-browser/chat-history/1.0.0` |
| **Control** | Reliable | half or full | `/pp-browser/dial-back/1.0.0`, ch0 |
| **Bulk** | Reliable | half | `/pp-browser/chat-blob/1.0.0` |
| **Realtime** | BestEffort | full | Opus/media relay media frames |
| **RealtimeControl** | Reliable | full | call-media hello/teardown on same call |

Class is fixed at OPEN and selects ADP QoS for the channel lifetime.

## L3 frame types

All multi-byte integers **little-endian** unless noted.

### Common header

| Field | Size | Notes |
|-------|------|-------|
| `frame_version` | 1 | `1` |
| `frame_type` | 1 | see below |
| `channel_id` | 4 | u32 |
| `channel_seq` | 4 | per-channel, per-direction; Reliable channels only |

### Frame types

| Value | Name | Payload |
|-------|------|---------|
| 0 | `Open` | `protocol_id` LenUtf8, `channel_class` u8, `flags` u16 |
| 1 | `OpenAck` | `result` u8 (0=ok) |
| 2 | `Data` | L4 bytes or fragment (see § Fragmentation) |
| 3 | `Close` | optional reason LenUtf8 |
| 4 | `Reset` | error code u32 |
| 5 | `Frag` | fragmentation meta + chunk (Reliable bulk/large txn) |

**LenUtf8:** `u32` LE byte count + UTF-8 (same profile as [WIRE_SCHEMAS § pp Binary Wire Profile](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088) but LE length for L3).

After L2 decrypt, parser validates `frame_version`, dispatches on `frame_type`, enforces per-channel ordering for Reliable.

## Channel lifecycle

```text
        OPEN ──────────────► OPEN_ACK (ok)
          │                      │
          ▼                      ▼
       DATA ◄────────────────► DATA
          │                      │
    CLOSE / RESET            CLOSE / RESET
          │                      │
          ▼                      ▼
       terminal               terminal
```

| Event | Scope |
|-------|--------|
| **RESET** | This channel only; drop queued outbound |
| **CLOSE** | Graceful; Reliable may drain |
| **Session fail** | All channels on peer |

### Failure propagation

Matches [STACK.md § Failure propagation](../../projects/adp/STACK.md#failure-propagation).

## Channel 0 — capability plane

Opened immediately after Session `Established`.

**Dialer (outbound link) opens channel 0** and sends a capability payload; **responder OpenAcks and replies** with its own payload on the same channel ([A016](../../projects/adp/DECISIONS.md#a016--channel-0--capability--identify-plane)). Wired from `PeerLinkManager::OnLinkEstablished`.

**Payload (Transactional / Control DATA):**

- `local_peer_id`
- `listen_multiaddrs[]`
- `protocols[]` supported (includes circuit / media-relay protocol ids when hosted)
- `capabilities` object (circuit_relay, media_relay, … — mirrors today’s Identify extensions; v1 binary codec carries flags via `protocols[]` until an explicit flags field is added)

Channel 0 may stay open (long-lived control) or close after exchange — implementation policy; default **long-lived, Reliable Control**.

Replaces libp2p Identify stream on the AMP path.

## Fragmentation (L3)

**L1 max ADP payload 1200 B is fixed.** L3 fragments large L4 messages:

| Field (Frag payload) | Size |
|----------------------|------|
| `msg_id` | 8 |
| `frag_index` | 2 |
| `frag_count` | 2 |
| `total_len` | 4 |
| `chunk` | remainder |

- **Reliable** channels: reassemble in order; dup `msg_id`+index dropped; incomplete assembly times out (default **30 s**).
- **BestEffort** (optional, video): may skip missing frags; out of scope for v1 control paths.

Max reassembled size defaults match today’s stream caps:

| Policy | Default max |
|--------|-------------|
| Control JSON | 256 KiB |
| Chat blob | per `Libp2pExecutorLimits` |
| Call media | per `CallMediaIoPolicy` |

## Channel policies (port StreamIoPolicy)

| Factory | Class | `max_outbound` | Drop | `read_once` | Read timeout |
|---------|-------|----------------|------|-------------|--------------|
| `CallMediaChannelPolicy` | Realtime | 64 | Oldest | no | off |
| `MediaRelayHopChannelPolicy` | Realtime | 1 | Oldest | no | off |
| `MediaRelayClientChannelPolicy` | Interactive | 4 | Oldest | no | off |
| `ControlJsonChannelPolicy` | Control | 1 | Never | yes | 8 s |
| `ChatBlobChannelPolicy` | Bulk | 1 | Never | configurable | 8 s |

Timers require `MeshPump` io executor (same as `timer_executor` on `DuplexFrameSession` today).

## L4 mapping (unchanged payloads)

| `protocol_id` | L4 shape |
|---------------|----------|
| `/pp-browser/chat/1.0.0` | One DATA = `RelayEnvelope` JSON; ack DATA = `{"ok":true}` |
| `/pp-browser/chat-history/1.0.0` | request JSON → response JSON |
| `/pp-browser/chat-blob/1.0.0` | JSON meta → ciphertext or error JSON |
| `/pp-browser/call-media/1.0.0` | Reliable hello + BestEffort media AEAD frames (see [Call-media bundle](#call-media-bundle)) |
| `/pp-browser/media-relay/1.0.0` | duplex realtime + control |
| `/pp-browser/circuit-relay/1.0.0` | tunnel setup → forwarded L3 frames ([A019](../../projects/adp/DECISIONS.md#a019--circuit-relay--channel-tunnel)) |
| `/pp-browser/dial-back/1.0.0` | short JSON |

Decode rules: **exact consume** for binary L4; JSON unknown-field policy per [WIRE_SCHEMAS § Unknown-field](WIRE_SCHEMAS.md#unknown-field-policy-d073).

## Call-media bundle

One call attempt = **`call_id`-keyed control + media channel pair** on an existing peer Session (not a byte stream).

| Channel role | Class | Role |
|--------------|-------|------|
| Outbound / inbound control | Reliable `RealtimeControl` | JSON hello / hello_ack (provisional dual-dial may have both briefly) |
| Media | BestEffort `Realtime` | length-prefixed AEAD frames (same crypto as libp2p path) |

**Glare (dual offerer dial):** when both peers negotiate the same `call_id`, the **higher base58 PeerId** keeps outbound control; the lower PeerId abandons outbound (`CloseQuiet`) and adopts inbound. Rejected inbound receives `hello_ack` with `"error":"glare"`. Close of a non-winning inbound during `OutboundHello` must not tear down the winning outbound — see [A021](../../projects/adp/DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime).

Admit rules are pure (`CallMediaBundleLogic`); L4 runs on **`MeshRuntime`** io thread.

## Circuit tunnel (v1)

Relay hosts `/pp-browser/circuit-relay/1.0.0`. After a JSON bridge handshake, the relay **splices opaque L4 DATA bodies** between the client circuit channel and a channel opened to the target on `target_protocol` (parity with today’s `StreamBridge`). Each hop still has its own AMP Session (A↔R, R↔B); the relay does not terminate an A↔B Session. Nested end-to-end Session through the tunnel ([A019] blind L2 ciphertext) remains a future refinement.

### Bridge request (first DATA on circuit channel)

```json
{
  "v": 1,
  "op": "bridge",
  "timeout_ms": 8000,
  "target_peer_id": "<base58>",
  "target_multiaddr": "/ip4/.../udp/.../adp/1.0.0/p2p/...",
  "target_protocol": "/pp-browser/call-media/1.0.0"
}
```

`target_multiaddr` and/or `target_peer_id` required. `target_protocol` defaults to the circuit protocol id when omitted.

### Bridge result (second DATA, before splice)

```json
{ "v": 1, "ok": true, "resolved_multiaddr": "..." }
```

or `{ "v": 1, "ok": false, "error": "..." }`. On success, further DATA bodies are forwarded bidirectionally until either channel closes.

Channel policy: `CircuitTunnelChannelPolicy` (Reliable Control, not `read_once`). Admission uses the same contact/scope rules as libp2p circuit ([RELAY_SCOPE](../../projects/p2p-mesh/RELAY_SCOPE.md)).

Runtime: **`CircuitTunnelCoordinator`** on `MeshRuntime` — non-blocking `StartBridge` + completion callback ([A022](../../projects/adp/DECISIONS.md#a022--circuit-tunnel--non-blocking-coordinator-on-meshruntime)). L4 must not nest `Pump` / `IoPumpUntil`.

### Nested Session carrier ([A024](../../projects/adp/DECISIONS.md#a024--amp-call-media-over-circuit--nested-session))

For Amp **call-media** over circuit, the bridged channel is **not** the product L4 pipe. Outer `target_protocol` is `/pp-browser/amp-circuit-carrier/1.0.0`. After splice:

1. A and B run **inner MSH** over the carrier (`PeerLink` carrier mode; non-chunked `AmpAdpCarrier` MSH/sealed frames as ChannelSession DATA).
2. Inner `Session` + `ChannelMux` becomes a normal PeerLink; `OpenChannel` opens A021 control+media on that mux.
3. Outer policy: `CircuitCarrierChannelPolicy` (BestEffort Realtime, large outbound queue) so media FRAG bursts are not stalled by ADP `reliable_window`.

Media-relay SoftMigrate continues to adopt one opaque channel (5c); nested Session is call-media only. See [CALL_MEDIA_CIRCUIT.md](../../projects/adp/CALL_MEDIA_CIRCUIT.md).

## Three objects (do not collapse)

| Object | Type |
|--------|------|
| **ChannelSession** | L3 pipe — replaces `DuplexFrameSession` |
| **PeerLinkManager** | peer link — replaces dial + warm in `PeerSessionManager` |
| **Domain session** | CallLifecycle, thread store, relay participant |

## Threading

`ChannelSession` is **io-thread affine**. Heavy work posts to worker pool ([THREADING.md](../architecture/THREADING.md)).

## Testing requirements

| Suite | Coverage |
|-------|----------|
| OPEN/ACK/CLOSE/RESET | single + parallel channels |
| QoS map | Reliable channel never sends BE ADP |
| Fragmentation | loss, reorder, dup, timeout |
| ch0 | capability round-trip |
| Policy | drop oldest, read_once, 8 s timeout |
| Isolate | RESET does not kill sibling channel |

## Related ADRs

[A014](../../projects/adp/DECISIONS.md#a014--one-association-per-peer-pair) · [A016](../../projects/adp/DECISIONS.md#a016--channel-0--capability--identify-plane) · [A018](../../projects/adp/DECISIONS.md#a018--fragmentation-at-l3-not-l1) · [A019](../../projects/adp/DECISIONS.md#a019--circuit-relay--channel-tunnel) · [A020](../../projects/adp/DECISIONS.md#a020--single-transport-entry-per-protocol)
