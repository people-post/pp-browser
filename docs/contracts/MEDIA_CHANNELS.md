# Media channels

**Tier:** contract

What the channel byte of a [media frame](WIRE_SCHEMAS.md#media-frames) means, on every path (`channel_id`, `u16` in code, one byte on the wire). Relays forward and filter on it; they never look inside the payload.

## Layout

One byte: **high nibble = track kind, low nibble = level.**

| Kind | Value | Level |
|------|-------|-------|
| Audio | `0` | always `0` → channel `0x00` |
| Video | `1` | `1`–`15` → channels `0x11`–`0x1F` |

Other kinds are reserved. On the relay's `u16` field the high byte is `0`.

## Video levels

A level is an **opaque ordered integer**: higher = more bits. Only the order is shared — what a level means (size, frame rate, bitrate) is the sending client's choice. In practice at most two levels are used; `1` is the default single level (calls send only level `1`).

Guidance for clients (not a contract): level `1` ≈ 360p / ~400 kbps, level `2` ≈ 720p / ~1.2 Mbps.

## Channel type (relay QoS)

Senders set the relay frame's `channel_type` from the kind: audio → `ReliableOrdered`, video → `LatestLossy`. Only `LatestLossy` changes what a relay does: it drops a stale non-keyframe for a subscriber that already has a newer one. `ReliableOrdered` is a label — the frame is forwarded like any other, and it is not resent. Call audio is best effort on every normal path, and the codec (Opus FEC, loss concealment, the jitter buffer) covers losses: [V063](../../projects/p2p-av-calls/DECISIONS.md#v063--call-audio-is-best-effort-the-codec-recovers-losses).

## Level negotiation (broadcast publishers)

A publisher offers the levels it can produce in the `media_relay` quote; the relay answers with the levels it will carry and drops, at ingest, any other channel from that participant (video of another level, or a reserved kind). Wire fields: [WIRE_SCHEMAS § media_relay control](WIRE_SCHEMAS.md#media_relay-control-pp-browserdatagram-relay100). Rules: [peer-scoped-broadcast B009](../../projects/peer-scoped-broadcast/DECISIONS.md#b009--video-levels-opaque-ordered-integers-negotiated-per-relay-at-attach).

Code: pp-cpp-mesh `mesh/types/MediaChannel.h`.
