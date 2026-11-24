# AMP Channel — the protocols on channels

The L3 contract (channel ids and classes, frames, lifecycle, channel 0, fragmentation, channel policies, the nested Session carrier, ownership, threading) moved to pp-cpp-amp: [docs/contracts/AMP-CHANNEL.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md) ([CR026](../../projects/capability-repos/DECISIONS.md#cr026--pp-cpp-amp-takes-the-stacks-design-and-the-contracts-it-alone-implements)). What stays here runs on those channels: the `protocol_id` map, the app's call-media bundle and pp-cpp-mesh's circuit tunnel requests. Each moved section keeps its heading at the end ([§ Moved to pp-cpp-amp](#moved-to-pp-cpp-amp)), pointing at its new place, so old links resolve.

**Stack:** [pp-cpp-amp STACK](https://github.com/people-post/pp-cpp-amp/blob/main/docs/STACK.md) · L4 kinds [L4_PROTOCOL_KINDS.md](L4_PROTOCOL_KINDS.md)

## L4 mapping (unchanged payloads)

| `protocol_id` | L4 shape |
|---------------|----------|
| `/pp/rpc/chat/1.0.0` | One DATA = `RelayEnvelope` JSON; ack DATA = `{"ok":true}` |
| `/pp/rpc/history/1.0.0` | request JSON → response JSON |
| `/pp/blob/1.0.0` | JSON meta → ciphertext or error JSON |
| `/pp/realtime/1.0.0` | Reliable hello + BestEffort media AEAD frames (see [Call-media bundle](#call-media-bundle)) |
| `/pp-mesh/datagram-relay/1.0.0` | Reliable control JSON (quote / accept / attach / subscribe); media too while the participant has no data lane |
| `/pp-mesh/media-relay-data/1.0.0` | A participant's data lane to the hop: `MediaDataFrame`s both ways, BestEffort, the oldest dropped (`MediaRelayDataChannelPolicy`) |
| `/pp-mesh/circuit/1.0.0` | tunnel setup → forwarded L3 frames ([A019](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a019--circuit-relay--channel-tunnel)) |
| `/pp-mesh/reach/1.0.0` | short JSON |

Decode rules: **exact consume** for binary L4; JSON unknown-field policy per [WIRE_SCHEMAS § Unknown-field](WIRE_SCHEMAS.md#unknown-field-policy-d073).

## Call-media bundle

One call attempt = **`call_id`-keyed control + media channel pair** on an existing peer Session (not a byte stream).

| Channel role | Class | Role |
|--------------|-------|------|
| Outbound / inbound control | Reliable `RealtimeControl` | JSON hello / hello_ack (provisional dual-dial may have both briefly), path migration, heartbeats, `bye` |
| Media | BestEffort `Realtime` | length-prefixed AEAD frames (same crypto as libp2p path) |

**Glare (dual offerer dial):** when both peers negotiate the same `call_id`, the **glare winner** keeps outbound control — the offerer beats the answerer; equal roles fall back to the **higher base58 PeerId** (`LocalWinsCallMediaGlareForRoles`). The loser abandons outbound (`CloseQuiet`) and adopts inbound. Rejected inbound receives `hello_ack` with `"error":"glare"`. Close of a non-winning inbound during `OutboundHello` must not tear down the winning outbound — see [A021](../../projects/adp/DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime).

Admit rules are pure (`CallMediaBundleLogic`); L4 runs on **`MeshRuntime`** io thread.

A second `hello` for a call that is already live is refused (`hello_ack` `"error":"busy"`) on its **own** channel; the live bundle's channels are never touched.

### Path migration (make-before-break — call-path-resilience k3)

A live call can move to another Connected link to the same peer (e.g. a punched direct link while it runs on a relay carrier, or onto a relay carrier when the direct path stops delivering). Either end may start one; when both do at once the **glare winner**'s goes ahead and the other end yields (abandons its own, answers the winner's). A `migrate` arriving while this end already accepted one, or is still releasing an old path, is refused `busy`. Frames are link-portable (the AEAD AAD carries no path term), so no re-key.

| Step | Channel | Message |
|------|---------|---------|
| 1 | new outbound control on the candidate link | `{"v":1,"type":"migrate","call_id":…,"media_epoch":…,"path_gen":n+1}` |
| 2 | same | `{"v":1,"type":"migrate_ack","ok":true,"path_gen":n+1}` — or `ok:false`, `error` ∈ `no live call` / `media_epoch` / `busy` / `path_gen` / `same path` / `link gone` |
| 3 | new media channel on the candidate link (driver opens it) | — each side switches TX to the new path when its end of this channel is bound; RX accepts every path of the call (seq de-dupe per media channel) |
| 4 | new control (driver → peer) | `{"v":1,"type":"path_release","path_gen":n}` once media arrived on the new path and ≥ 1 s passed (≤ 5 s) |
| 5 | same | `{"v":1,"type":"path_release_ack","path_gen":n}` — both close the old path's channels; closes on a released / retiring path are never failures |

The call stays on its path when the candidate fails before step 3: `migrate_ack` refusal, the candidate link or channel lost, or **no answer in 5 s** (`kCallMediaMigrateTimeoutMs`: the `migrate` or its ack lost, or the peer gone). If the new path dies **after** step 3 but before step 5 (a punched link lost to the dual-dial election a moment after the switch), both ends go back onto the retiring path — it is still bound and alive — and report the change like any failover. A lost candidate link, or such a fall-back, lets the driver's automatic relayed → direct move try another direct link at once (the lost one cannot be retried; an unused cold link lives only ~5 s). `path_gen` counts paths within the call (0 = the one it started on).

### Path liveness and failover (call-path-resilience k4)

- **Heartbeat** on each path's control channel (either direction): `{"v":1,"type":"hb","active":true}` every 500 ms on the sender's active path, `{"v":1,"type":"hb"}` every 10 s on its standby. Anything received on a path (heartbeat, control, media) proves it alive. Older peers ignore the type.
- **Standby:** a released path (step 5) stays bound as the call's warm **standby** instead of closing — one per call, a relayed one preferred. Its link stays up because the call's control channel asks for the **hot** keepalive tier while it is open (`CallMediaControlChannelPolicy`, amp `ChannelPolicy::keepalive` — K008), at both ends; the tier goes when the call's channels close and leaves whatever else holds the link (chat's warm mark) as it was. A standby carries nothing until a failover: without it a direct standby went cold and was evicted 5 s after it came up.
- **Failover** (no handshake — the standby's channels are bound and RX takes every path): TX moves to the standby when the active path's link is lost, or when it has been silent **1.5 s** while the peer is known to heartbeat (a muted mic still heartbeats and sends silence frames). A standby silent 25 s is not taken. After a failover, silence alone does not switch again for 3 s.
- **Follow:** an `active:true` heartbeat arriving on this end's standby means the peer moved there — this end switches too.
- **No path left** (active link lost, no standby): the call is kept — MediaReady on a dead path — for a **30 s reconnect window** (`kCallMediaReconnectWindowMs`). A `migrate` onto any new link to the peer (the offerer reaches it again) brings it back; the window running out drops the call: the leg reports `CallMediaErr::Dropped` (K014 — a call that was connected, never a connect failure), as it does for any end of a MediaReady leg under the call but the peer's `bye`. A fresh `hello` for the same call (a peer from before k4 re-dialing) replaces the reconnecting bundle.
- **Quiet rebind** (k7): when the path is lost while the peer is still Connected on another link (typically a simultaneous punch: both ends' associations came up and the dual-dial election dropped the one the call had bound), the glare winner migrates onto that link at once and the loss is not reported. The product hears of it (`on_path_lost` → `Reconnecting…`) only if the call is still without a path **1 s** later. The lost path leaves nothing behind: no retiring or standby entry.
- **Standby add** (k6, K003): `{"v":1,"type":"path_add","call_id","media_epoch","path_gen"}` runs the migration handshake (`migrate_ack`, media channel) but the new path becomes the call's **standby** — TX stays on the active path, nothing is released. Refused (`migrate_ack` `ok:false`, `standby present`) when the call has a standby already; unanswered, the add times out (5 s) and the call is unchanged. The offerer adds a relayed standby to a call Live on a direct / punched path (5 / 20 / 60 s attempts).
- **Roles:** the glare winner is the offerer. A bundle created from the peer's `hello` takes the complementary role, and a local leg that joins it (the offerer's media started after the answerer's hello arrived) sets its own role, so exactly one end drives.

### Ending a call (call-path-resilience K014)

- **Bye:** an end whose call is over on its device (left, ended, superseded — the call closed there) sends `{"v":1,"type":"bye"}` on its active path's control channel, then closes the call's channels. Best-effort, no ack; the call's end itself is the `call_leave` on the control lane. Only a live (MediaReady) call with a path says it; a reconnecting one has nothing to say it on.
- **Receiving `bye`** (on any path of the call): the leg ends at once as the peer's end of the call — not a failure, no reconnect window. Its channels' close that follows finds nothing.
- **A close without `bye`** — `peer_close` / `peer_reset` on the active path of a live call — is a **lost path**, handled like a dead link: back onto a retiring path, failover onto the standby, or the reconnect window (the offerer re-anchors, the answerer accepts its `migrate`). An end that lost its path (silence) and closed it therefore never hangs the other end up. A peer from before K014 hangs up without `bye`: the other end shows `Reconnecting…` until that peer's `call_leave` ends the call.
- **Media CLOSE first:** the media channel is best-effort, so its CLOSE can overtake the reliable `bye`. When the peer closes the active path's media channel while the path's control channel is still up, the path drops the media channel and waits **1.5 s** (`kCallMediaByeAwaitMs`) for that control channel's `bye` (the call ended) or its close (a lost path); with neither, the path is lost. Meanwhile the silence rules switch nothing (the wait's end decides; a lost link still counts at once) and the call's frames are dropped without an error.
- Peers from before K014 ignore the unknown `type`.

## Circuit tunnel (v1)

Relay hosts `/pp-mesh/circuit/1.0.0`. After a JSON bridge handshake, the relay **splices opaque L4 DATA bodies** between the client circuit channel and a channel opened to the target on `target_protocol` (parity with today’s `StreamBridge`). Each hop still has its own AMP Session (A↔R, R↔B); the relay does not terminate an A↔B Session. Nested end-to-end Session through the tunnel ([A019] blind L2 ciphertext) remains a future refinement.

### Bridge request (first DATA on circuit channel)

```json
{
  "v": 1,
  "op": "bridge",
  "timeout_ms": 8000,
  "target_peer_id": "<base58>",
  "target_multiaddr": "/ip4/.../udp/.../adp/2/p2p/...",
  "target_protocol": "/pp/realtime/1.0.0"
}
```

`target_multiaddr` and/or `target_peer_id` required. `target_protocol` defaults to the circuit protocol id when omitted.

Optional `"standby_priority": "low" | "medium" | "high"` marks a call's **standby** circuit (call-path-resilience K003); omitted for a primary circuit. A relay admits standby circuits against its standby capacity (default 64, `CircuitRelayServer::SetStandbyLimits`) — `low` below 50 %, `medium` below 80 %, `high` up to 100 % — and at most 4 per dialer PeerId; otherwise it answers `{"ok":false,"error":"relay busy: standby refused"}`. Primary circuits are never refused for standby load. An unknown value reads as `low`; relays without it ignore the field. Standby circuits are marked so relay metering (none yet) can leave them free until a failover (K009).

### Reserve request (answerer park; double-NAT)

Answerer opens a circuit channel and sends:

```json
{ "v": 1, "op": "reserve", "timeout_ms": 30000 }
```

Relay acks `{ "v": 1, "ok": true, "op": "reserve" }` and keeps the PeerLink so a later `bridge` to that PeerId can `EnsureAssociation` without dialing into the answerer’s NAT. Client API: `CircuitClientCoordinator::StartReserve`. TTL / `CancelTunnel` / channel close clears the park.

### Bridge result (second DATA, before splice)

```json
{ "v": 1, "ok": true, "resolved_multiaddr": "..." }
```

or `{ "v": 1, "ok": false, "error": "..." }`. On success, further DATA bodies are forwarded bidirectionally until either channel closes.

Channel policy: `CircuitTunnelChannelPolicy` (Reliable Control, not `read_once`). Admission uses the same contact/scope rules as libp2p circuit ([RELAY_SCOPE](../../projects/p2p-mesh/RELAY_SCOPE.md)).

Runtime: **`CircuitClientCoordinator`** (dialer / answerer: `StartBridge`, `StartReserve`) and **`CircuitRelayServer`** (relay: `bridge` / `reserve` answers) on `MeshRuntime` — non-blocking `StartBridge` + completion callback ([A022](../../projects/adp/DECISIONS.md#a022--circuit-tunnel--non-blocking-coordinator-on-meshruntime)). L4 must not nest `Pump` / `IoPumpUntil`.

## Moved to pp-cpp-amp

The L3 sections, under their old headings so that links to them still land here:

### Role

[AMP-CHANNEL.md § Role](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#role)

### Channel identifiers

[AMP-CHANNEL.md § Channel identifiers](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#channel-identifiers)

### Channel classes

[AMP-CHANNEL.md § Channel classes](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#channel-classes)

### L3 frame types

[AMP-CHANNEL.md § L3 frame types](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#l3-frame-types)

### Common header

[AMP-CHANNEL.md § Common header](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#common-header)

### Frame types

[AMP-CHANNEL.md § Frame types](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#frame-types)

### Channel lifecycle

[AMP-CHANNEL.md § Channel lifecycle](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#channel-lifecycle)

### Failure propagation

[AMP-CHANNEL.md § Failure propagation](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#failure-propagation)

### Channel 0 — capability plane

[AMP-CHANNEL.md § Channel 0 — capability plane](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#channel-0--capability-plane)

### Fragmentation (L3)

[AMP-CHANNEL.md § Fragmentation (L3)](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#fragmentation-l3)

### Channel policies (port StreamIoPolicy)

[AMP-CHANNEL.md § Channel policies (port StreamIoPolicy)](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#channel-policies-port-streamiopolicy)

### Nested Session carrier ([A024](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a024--amp-call-media-over-circuit--nested-session))

[AMP-CHANNEL.md § Nested Session carrier (A024)](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#nested-session-carrier-a024)

### Three objects (do not collapse)

[AMP-CHANNEL.md § Three objects (do not collapse)](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#three-objects-do-not-collapse)

### Ownership hierarchy ([A027](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy))

[AMP-CHANNEL.md § Ownership hierarchy (A027)](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#ownership-hierarchy-a027)

### Threading

[AMP-CHANNEL.md § Threading](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#threading)

### Testing requirements

[AMP-CHANNEL.md § Testing requirements](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#testing-requirements)

### Related ADRs

[AMP-CHANNEL.md § Related ADRs](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md#related-adrs)
