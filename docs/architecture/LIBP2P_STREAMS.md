# Libp2p stream framing & failure handling

**Tier:** architecture

How pp-browser exchanges bytes on libp2p streams: stack layers, on-wire shapes, protocol exchanges, and how shorter / longer / hung frames are handled. Normative **application** payloads remain in [WIRE_SCHEMAS.md](../contracts/WIRE_SCHEMAS.md) and [MESSAGE_ENCRYPTION.md](../contracts/MESSAGE_ENCRYPTION.md). Product messaging flow: [P2P_MESSAGING.md](P2P_MESSAGING.md). Fork notes: [LIBP2P_UPSTREAM.md](LIBP2P_UPSTREAM.md).

## Production host stack

`Libp2pHost` builds an ExplicitHost with **TCP + Noise (`/noise-mlkem768/1.0.0`) + Yamux** only (QUIC exists in the fork but is not wired on the live path).

```
TCP
 └─ Noise XXkem frames:  u16-BE length + ciphertext (+16 B tag), max 65535
      └─ Yamux frames: 12-byte header + payload (length u32-BE)
           └─ App stream: u64-BE length + body
                └─ (chat/history) UTF-8 JSON RelayEnvelope / history messages
                     └─ body.e2e.payload_b64 → E2E blob → ChatPayload plaintext
```

App code sees only `Host` / `CapableConnection` / `Stream`. Transport choice is multiaddr-driven (`TransportAdaptor`); TCP synthesizes a muxed secure connection via Upgrader → Noise → Yamux. See [NETWORKING.md](NETWORKING.md).

## App stream framing (`StreamFrameIo`)

Shared helper for control-plane and media protocols. Yamux / Noise / TCP queues stay **inside the fork**; app code uses one pipe type.

| Piece | API |
|-------|-----|
| Binary frame | `EncodeLengthPrefixedFrame` / `BlockingReadLengthPrefixedFrame` / `AsyncLengthPrefixedReader` / `DuplexFrameSession` |
| Policy | `StreamIoPolicy` + factories (`CallMediaIoPolicy`, `MediaRelayHopIoPolicy`, `MediaRelayClientIoPolicy`, `ControlJsonIoPolicy`) |
| JSON wrapper | `StreamJsonFrame` — blocking helpers (dial-back / circuit / tests); chat/history use `DuplexFrameSession` |
| Config | `LengthPrefixedFrameConfig` (`max_frame_bytes`, `allow_empty_body`, `read_timeout`, `timer_executor`) |

**On-wire shape:**

```
[8 bytes: payload length, big-endian u64][N bytes: body]
```

Defaults:

| Setting | Default | Notes |
|---------|---------|--------|
| `max_frame_bytes` | 256 KiB | Chat/history pass `kMaxRelayEnvelopeJsonBytes` (256 KiB) |
| `read_timeout` | **0** (off) on raw `StreamFrameIo` | Media duplex leaves this off so silence between frames is OK |
| Control-plane JSON | **8 s** (`kDefaultControlFrameReadTimeout`) | `ControlJsonIoPolicy` / blocking JSON default |
| Async / duplex timer | requires `timer_executor` (host `IoExecutor`) | Without it, `read_timeout` is ignored on async paths |

Exact byte assembly uses `libp2p::read` (`basic/read.hpp`): loop `readSome` until `out.size()` bytes, or error. **No deadline inside `libp2p::read` itself** — deadlines are enforced by framing helpers (below).

### Three objects (do not collapse)

| Object | Question | Lives as long as | Owns |
|--------|----------|------------------|------|
| **Pipe** (`DuplexFrameSession`) | Can I read/write frames on *this* Yamux stream? | Until reset / EOF | Framing, outbound cap, drop vs fail, full-duplex, `read_once` |
| **Peer link** (`PeerSessionManager`) | Can I open a stream to *this device*? | Warm TTL / connection | Dial, backoff, circuit vs direct |
| **Domain session** | What are we *doing*? | Product lifetime | Call (`CallLifecycle`), thread store, hop `HostParticipant` |

State that must survive `stream->reset()` does **not** live on the pipe. Classify inbound frames and dispatch to the stream’s handler immediately — no process-wide inbound heap. Jitter / SQLite / roster stay on domain consumers.

`StreamIoPolicy` (app outbound only):

| Factory | Class | Cap | Drop | Duplex | Timeout |
|---------|-------|-----|------|--------|---------|
| `CallMediaIoPolicy` | Realtime | 64 | Oldest | full | off |
| `MediaRelayHopIoPolicy` | Realtime | 1 | Oldest | full | off |
| `MediaRelayClientIoPolicy` | Interactive | 4 | Oldest | full | off |
| `ControlJsonIoPolicy` | Control | 1 | Never | half; `read_once` | 8 s + `IoExecutor` |

Muxer-internal queues (unchanged): Yamux stream `WriteQueue` + `ReadBuffer`, connection write queue. Do not reimplement them at app level.

## Protocol exchanges

| Protocol | Service | Exchange |
|----------|---------|----------|
| `/pp-browser/chat/1.0.0` | `Libp2pDirectChatService` | One short stream per message: write `RelayEnvelope` JSON → read `{"ok":true}` ack (`DuplexFrameSession` + `ControlJsonIoPolicy`) |
| `/pp-browser/chat-history/1.0.0` | `Libp2pChatHistoryService` | Write `ChatHistoryRequest` JSON → read `ChatHistoryResponse` JSON (same pipe; SQLite `Serve` on worker) |
| `/pp-browser/chat-blob/1.0.0` | `Libp2pChatBlobService` | Write `ChatBlobRequest` JSON → read ciphertext **or** JSON error ack (fetch); push: JSON request → ciphertext frame → JSON ack |
| dial-back / circuit-relay | same JSON framing family | Control frames; blocking JSON still used for handshake / `StreamBridge` |
| media-relay / call-media | `DuplexFrameSession` | Ongoing length-prefixed frames; **no** per-frame read timeout by default |

Chat/history **frame R/W** runs on the host `io_context`. Parse / `ChatHistoryResponder::Serve` / inbound envelope delivery run on `PostLibp2pWorker` (Normal lane).

## Application payload (inside the JSON frame)

After decrypt, AEAD plaintext is binary **ChatPayload** ([WIRE_SCHEMAS](../contracts/WIRE_SCHEMAS.md)):

| Offset | Field |
|--------|--------|
| 1 B | `payload_version` = 1 |
| 1 B | `content_type` |
| var | `text` as **LenUtf8** (`u64` BE + UTF-8) |
| var | type tail |

Decode requires **exact consume** (`exactEnd`) — trailing bytes rejected. Plaintext cap: `kMaxE2ePlaintextBytes` (128 KiB).

E2E blob layout: [MESSAGE_ENCRYPTION.md](../contracts/MESSAGE_ENCRYPTION.md) (`[version:1][nonce:24][ciphertext+tag]`).

## Lower-layer framing (fork)

| Layer | Framing | Caps / notes |
|-------|---------|----------------|
| **Noise** | `[u16 BE len][payload]` | `kMaxMsgLen` = 65535; plaintext max 65519 (minus 16 B tag) |
| **Yamux** | 12-byte header (`version`, `type`, `flags`, `stream_id`, `length`) + data | Initial window 256 KiB; ping / idle-no-streams for connection hygiene — **not** per-app-frame deadlines |
| **Multiselect** | uvarint length + message + `\n` | Protocol negotiation; `kMaxMessageSize` 65535 |
| **Stock helpers** (`MessageReadWriterUvarint`, gossip, …) | uvarint / protobuf-shaped | Used by identify/kad/gossip — **not** by `/pp-browser/chat*` |
| **QUIC** (fork only) | Native QUIC streams + TLS | Same `Stream` API; not on `Libp2pHost` production path |

## Failure handling

### Shorter than expected

| Layer | Behavior |
|-------|----------|
| App `u64` frame | Wait for exact header (8) then exact body (N). Peer close/EOF → `"Failed to read … header/body"`; stream closed |
| ChatPayload / LenUtf8 | Archive decode fail and/or `exactEnd` fail |
| Noise / Yamux | Wait for full declared frame; connection death ends the wait |

No “accept truncated and pad” path.

### Longer / oversized

| Case | Behavior |
|------|----------|
| Declared length `> max_frame_bytes` | Reject **immediately after header**, do not read body; `stream->reset()`; error `"length-prefixed frame too large"` / duplex `frame_too_large` |
| Write path too large | Reject before send (`"json frame too large"`) |
| Noise write `> 65535` | Rejected |
| ChatPayload trailing bytes | Rejected (`exactEnd`) |
| Extra bytes after a valid N-byte body | Not part of this frame. One-shot chat closes after the exchange; duplex treats leftovers as the next header |

### Hanging / incomplete reads

Framing-layer deadlines (not raw epoll — Asio’s reactor already uses epoll/kqueue under `io_context`):

| Path | Budget | On expiry |
|------|--------|-----------|
| Control JSON read (`ControlJsonIoPolicy` / blocking JSON) | **8 s** per frame (header+body) | `stream->reset()`; `"length-prefixed frame read timed out"` |
| Direct chat **send** (open + write + ack) | **4 s** outer | Reset active stream so the duplex read does not hang |
| Direct chat **inbound** | 8 s frame read | Same as control JSON (`DuplexFrameSession`) |
| Chat-history **Fetch** | **8 s** total for open + response body | Reset stream on timeout; remaining budget for duplex `read_timeout` |
| Async / duplex with `read_timeout` + `timer_executor` | Per config | Timer fires → take callback → `reset` → error / `CloseSession("read_timeout")` |
| Media duplex (default) | None per frame | Cancel / session close only; silence between frames is allowed |
| Yamux ping / idle | Connection-level | Does not bound a mid-frame app read |

Timeout errors are delivered **before** `stream->reset()` on async paths so an in-flight `libp2p::read` completion cannot clear the callback first.

### Empty frames

Rejected unless `allow_empty_body` (media-relay may allow empty). Empty control frames → reset + error.

## Key code

| Concern | Location |
|---------|----------|
| Frame IO | `src/base/p2p/StreamFrameIo.*` (`StreamIoPolicy`, `DuplexFrameSession`) |
| JSON frames | `src/base/p2p/StreamJsonFrame.*` |
| Direct chat | `src/feature/messaging/Libp2pDirectChatService.*` |
| Chat history | `src/feature/messaging/Libp2pChatHistoryService.*` |
| Chat blob | `src/feature/messaging/Libp2pChatBlobService.*` |
| Limits | `src/base/messaging/MessagingLimits.h` |
| Exact read | `src/lib/libp2p/include/libp2p/basic/read.hpp` |
| Noise caps | `src/lib/libp2p/include/libp2p/security/noise/crypto/state.hpp` |
| Tests | `src/base/people/tests/stream_frame_io_test.cpp` |

## Design notes

- **Frame deadlines belong in framing**, not SO_RCVTIMEO / raw epoll timeouts: the budget is “finish this length-prefixed message,” not “the TCP socket was idle.”
- Outer `future.wait_for` without `stream->reset()` is insufficient — it returns failure to the caller while the host io thread can remain blocked in `libp2p::read`.
- Do not enable a default per-frame timeout on media duplex; call-media / media-relay use cancel flags and session teardown instead.
