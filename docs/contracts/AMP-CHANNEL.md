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

**Requester sends (Transactional):**

- `local_peer_id`
- `listen_multiaddrs[]`
- `protocols[]` supported
- `capabilities` object (circuit_relay, media_relay, … — mirrors today’s Identify extensions)

**Responder sends OpenAck + capability payload.**

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

One call attempt = **one control + one media channel pair** on an existing peer Session (not a byte stream).

| Channel | Class | Role |
|---------|-------|------|
| Control | Reliable `RealtimeControl` | JSON hello / hello_ack on the wire |
| Media | BestEffort `Realtime` | length-prefixed AEAD frames (same crypto as libp2p path) |

**Glare (dual offerer dial):** when both peers `StartLeg` the same `call_id`, the **higher base58 PeerId** keeps its outbound control channel; the lower PeerId abandons outbound and adopts the peer's inbound hello. Rejected inbound controls receive `hello_ack` with `"error":"glare"`. Behavioral parity with libp2p `CallMediaSession` — see [A021](../../projects/adp/DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime).

L4 runs on **`MeshRuntime`** io thread; inbound hello handlers may run on a worker lane but all wire ops bounce back via `PostToIo`.

## Circuit tunnel (outline)

Relay opens paired tunnel channels to A and B. **Opaque L3 frames** (post-L2 ciphertext on each leg) forwarded by relay without decrypt.

Tunnel OPEN includes:

- `target_protocol_id`
- `target_peer_id`
- relay scope / quote refs (product fields)

Full wire in [p2p-mesh RELAY_SCOPE](../../projects/p2p-mesh/RELAY_SCOPE.md) — updated when tunnel ships.

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
