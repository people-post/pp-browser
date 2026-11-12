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

Senders set the relay frame's `channel_type` from the kind: audio → `Forward` (`0`), video → `LatestLossy` (`1`). It says only what a relay does with the frame: `Forward` — forwarded as it came; `LatestLossy` — a non-keyframe older than one the subscriber already has is dropped. Other values are refused. No frame is resent, on any path.

**The rule ([V065](../../projects/p2p-av-calls/DECISIONS.md#v065--call-audio-keeps-a-delay-budget-audio-before-video)).** Call audio keeps a delay budget: about 300 ms mouth to ear, never more than 400 ms. Inside it audio comes first, and a loss is recovered where it can be (Opus in-band FEC, the jitter buffer waiting for a late packet). Past it a frame is concealed (loss concealment), never waited for. Where audio and video wait in one queue, video is shed first and audio drops only its own frames.

**The receiver's jitter buffer** holds at most **400 ms**; its adaptive target runs 60–300 ms. Above the target it catches up by skipping decoded silence, never by playing faster. Past 400 ms it skips silence first, as much as the excess, and cuts speech to 400 ms only when the head of the buffer is speech (counted as speech drops). An outage is not jitter: a packet later than the target's top could hold, and the backlog behind it, do not raise the target. An underrun waits up to the target's top, then conceals; the packets of the concealed slots are late drops when they come.

**Two lanes on every connection (pp-cpp-mesh after v6.10.0).**
- **The data lane** (`/pp-mesh/media-relay-data/1.0.0`, best effort) carries a participant's media to and from the hop, both ways. **The control channel** (`/pp-mesh/datagram-relay/1.0.0`, reliable) carries quote, accept, attach and subscribe — never media. A hop drops media that comes on a control channel (`pp_media_relay_dropped_frames_total{reason="control_channel"}`).
- **Both ride the same link**, direct or, for a hop reachable only through a circuit, a link nested through the relay ([A024](../../projects/adp/CALL_MEDIA_CIRCUIT.md), as a 1:1 call's).
- **The attach is done once its lane is open.** A lane that is not open yet, or ended, is tried again (1, 2, 4, 8, then every 10 s); a frame sent meanwhile is dropped (client health `lane_wait_drops`), and a hop has nothing for a member without one (`reason="no_lane"`).
- **A hop that does not serve the lane is not a media hop:** the attach fails (`HopRefused`), and the caller picks another hop or path. No compatibility with hops older than the lane.

**Where media waits.** Nowhere in userspace: a best-effort send goes to the link in the same call, and its socket. Past capacity frames are lost in the kernel — a node's receive buffer ([STRESS_LAB](../ops/STRESS_LAB.md)), a phone link's queue — first in, first out. There only the sender's rate acts: video gives way first ([V024](../../projects/p2p-av-calls/DECISIONS.md#v024--adaptive-call-media-11-p2p-and-sfu-generic-relay-channels)'s ladder).

## Level negotiation (broadcast publishers)

A publisher offers the levels it can produce in the `media_relay` quote; the relay answers with the levels it will carry and drops, at ingest, any other channel from that participant (video of another level, or a reserved kind). Wire fields: [WIRE_SCHEMAS § media_relay control](WIRE_SCHEMAS.md#media_relay-control-pp-browserdatagram-relay100). Rules: [peer-scoped-broadcast B009](../../projects/peer-scoped-broadcast/DECISIONS.md#b009--video-levels-opaque-ordered-integers-negotiated-per-relay-at-attach).

Code: pp-cpp-mesh `mesh/types/MediaChannel.h` (the channel byte), `mesh/l4/shared/MediaFrame.h` (`MediaChannelType`).
