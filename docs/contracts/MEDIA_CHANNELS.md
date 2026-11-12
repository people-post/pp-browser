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

Senders set the relay frame's `channel_type` from the kind: audio → `ReliableOrdered`, video → `LatestLossy`. A relay drops a stale `LatestLossy` non-keyframe for a subscriber that already has a newer one, and sheds `LatestLossy` first where frames wait. `ReliableOrdered` is the wire's name for audio's class: such a frame is never resent and never skipped for a newer one.

**The rule ([V065](../../projects/p2p-av-calls/DECISIONS.md#v065--call-audio-keeps-a-delay-budget-audio-before-video)).** Call audio keeps a delay budget: about 300 ms mouth to ear, never more than 400 ms. Inside it audio comes first, and a loss is recovered where it can be (Opus in-band FEC, the jitter buffer waiting for a late packet). Past it a frame is concealed (loss concealment), never waited for. Where audio and video wait in one queue, video is shed first and audio drops only its own frames. Catch-up never plays faster; it skips silences first.

**Where media waits.**
- **A best-effort lane holds nothing.** The 1:1 `Realtime` channel and a member's data lane send in the same pump round, straight to the socket (the channel's 64-frame drop-oldest queue does not fill). Past capacity frames are lost in the kernel — a node's receive buffer ([STRESS_LAB](../ops/STRESS_LAB.md)), a phone link's queue — first in, first out. There only the sender's rate acts: video gives way first ([V024](../../projects/p2p-av-calls/DECISIONS.md#v024--adaptive-call-media-11-p2p-and-sfu-generic-relay-channels)'s ladder).
- **The reliable fallback queues** (below), in its link's mux, waiting for window.

**Which channel carries it (pp-cpp-mesh v6.8.0).**
- **The data lane:** each participant's media rides its data lane to the hop (`/pp-mesh/media-relay-data/1.0.0`, best effort), both ways.
- **While a hop serves lanes, media never goes on the reliable control channel.** A lane that is still opening, or that ended, is tried again while the attach lasts (1, 2, 4, 8, then every 10 s), and a frame sent meanwhile is dropped (client health `lane_wait_drops`). Reliable media under loss is resent and arrives later and later: on a past-capacity hop, p99 grew a second per second of run, to tens of seconds.
- **A hop that refuses the lane protocol** (older than it, OPEN_ACK "no handler") and a hop behind a circuit still carry media on the control channel. There a frame is dropped instead of queued once that channel's oldest waiting frame is past its kind's deadline: **video 50 ms, audio 150 ms** (pp-cpp-mesh v6.11.0; video 300 ms before). Frames already queued are numbered on the reliable channel and stay. The client counts `late_drops`; the hop counts `pp_media_relay_dropped_frames_total{reason="late"}`.
- **Why 50 ms and 150 ms:** video goes first, so the queue audio waits in holds little video. A call through a hop is already ~200 ms one-way; 150 ms more is about what audio's 400 ms leaves.

## Level negotiation (broadcast publishers)

A publisher offers the levels it can produce in the `media_relay` quote; the relay answers with the levels it will carry and drops, at ingest, any other channel from that participant (video of another level, or a reserved kind). Wire fields: [WIRE_SCHEMAS § media_relay control](WIRE_SCHEMAS.md#media_relay-control-pp-browserdatagram-relay100). Rules: [peer-scoped-broadcast B009](../../projects/peer-scoped-broadcast/DECISIONS.md#b009--video-levels-opaque-ordered-integers-negotiated-per-relay-at-attach).

Code: pp-cpp-mesh `mesh/types/MediaChannel.h`.
