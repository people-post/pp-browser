# L4 protocol kinds

**Tier:** contracts  
**Status:** Accepted design (2026-09-04) — [A028](../../projects/adp/DECISIONS.md#a028--l4-protocol-kinds--seven-conversation-shapes) · [N030](../../projects/p2p-mesh/DECISIONS.md#n030--adopt-l4-protocol-kinds-gate)  
**Stack:** [STACK.md](../../projects/adp/STACK.md) · L3 [AMP-CHANNEL.md](AMP-CHANNEL.md)

## Role

AMP L1–L3 already provide association, crypto, mux, QoS, and fragmentation. **L4 `protocol_id` strings name conversation shapes**, not product features. This document freezes the complete set of kinds so agents and humans do not mint a new `/pp-…/1.0.0` for every feature.

Wire `protocol_id` strings are **kind-aligned** (no dual advertise; product not released). Shipped ids: `/pp-browser/rpc/chat|rpc/history|blob|realtime|datagram-relay|circuit|reach/1.0.0` plus `/pp-mesh/directory|dht/1.0.0`. Kind **rpc** has two OPEN ids (chat vs history) — demux by `protocol_id`, not first-frame `op`. `reach` uses `/reach/1.0.0` (dial-back) and `/reach/punch/1.0.0` (punch SM).

## Gate — when to add a `protocol_id`

Add a new L4 `protocol_id` **only** when the conversation contract is new:

| Need a new id when… | Prefer ops / fields / composition when… |
|---------------------|----------------------------------------|
| Different QoS / duplex / lifetime (see [AMP-CHANNEL channel classes](AMP-CHANNEL.md#channel-classes)) | New message or envelope type |
| Different security posture (E2E session vs blind hop vs opaque splice) | New codec, media channel, or frame mark |
| Different failure model (one-shot RPC vs long-lived session machine — [V033](../../projects/p2p-av-calls/DECISIONS.md)) | New hop attach/pricing step |
| | New NAT / reachability trick |
| | New tunnel hop count or broker path |

**Do not** add `/pp-browser/call-signal/…`, A/V-specific relay forks, or per-feature tunnels. Prefer envelope `op`, `channel_type` / frame fields, `target_protocol` on circuit, and PeerLink policy.

**Multiple OPEN ids under one kind are OK** when separate services own the inbound handler (e.g. `rpc/chat` vs `rpc/history`). That is still one kind — not a license to mint a new *kind* per feature. Avoid first-frame `op` sniffing only to share a single OPEN string across unrelated handlers.

## Seven kinds (complete set)

A mesh that can host most apps needs these shapes — not more:

| Kind | Question it answers | Typical L3 class | Namespace |
|------|---------------------|------------------|-----------|
| **identify** | Who are you / what do you speak? | Control (ch0) | well-known channel 0 — not a product `protocol_id` |
| **discover** | How do I find names / PeerIds / hints? | Control | `/pp-mesh/*` |
| **reach** | Can we meet on the path? | Control | `/pp-browser/*` (infra) |
| **circuit** | Opaque live tunnel when direct fails | Realtime (+ control JSON) | `/pp-browser/*` (infra) |
| **rpc** | Reliable typed request/response + push | Transactional | `/pp-browser/*` (app) |
| **blob** | Content-addressed bulk bytes | Bulk | `/pp-browser/*` (app) |
| **realtime** | Duplex live media or blind datagram fan-out | Realtime + RealtimeControl | `/pp-browser/*` |

Namespaces: **`/pp-mesh/*`** = mesh infrastructure discovery; **`/pp-browser/*`** = app and infra conversation protocols advertised on ch0. Plumbing carriers (e.g. `amp-circuit-carrier`) are not product kinds.

### Kind contracts (summary)

| Kind | Contract |
|------|----------|
| **identify** | After Session Established, dialer opens ch0; both sides exchange PeerId, listen multiaddrs, `protocols[]`, capability flags ([A016](../../projects/adp/DECISIONS.md#a016--channel-0--capability--identify-plane)). |
| **discover** | Control JSON with `op` discrimination (list/find/store/…). DHT and directory may keep separate ids if state machines differ; same family. |
| **reach** | Short control probes and coordinated dial assist (dial-back, punch). Preference order stays publish → punch → circuit → fail — not parallel NAT toolkits. |
| **circuit** | Broker opens a live opaque tunnel; bridge JSON selects `target_protocol` for the far side ([A019](../../projects/adp/DECISIONS.md#a019--circuit-relay--channel-tunnel), [A024](../../projects/adp/DECISIONS.md#a024--amp-call-media-over-circuit--nested-session)). Multi-hop = **ops on the same kind**, not a new family. |
| **rpc** | Reliable half-duplex (or short full) typed messages; one-shot or short exchanges — not long-lived media SMs ([V033](../../projects/p2p-av-calls/DECISIONS.md)). Envelopes, history queries, future inbox assist share this shape. |
| **blob** | Large content by hash/meta; ciphertext or error; CDN may be secondary. Same contract for chat attachments, profile icons, documents. |
| **realtime** | Two postures under one kind: **E2E session** (call-media bundle: Reliable control + BestEffort media) and **blind hop** (media-relay / conceptual `datagram_relay` — content-agnostic fan-out, [N021](../../projects/p2p-mesh/DECISIONS.md#n021--generic-media_relay-framing-qos-channel-types)). |

## Current `protocol_id` → kind map

| Current id | Kind | Notes |
|------------|------|-------|
| Channel 0 capability plane | **identify** | [A016](../../projects/adp/DECISIONS.md#a016--channel-0--capability--identify-plane) |
| `/pp-mesh/directory/1.0.0` | **discover** | |
| `/pp-mesh/dht/1.0.0` | **discover** | Separate id while Kademlia SM differs |
| `/pp-browser/reach/1.0.0` | **reach** | Dial-back probe |
| `/pp-browser/reach/punch/1.0.0` | **reach** | Punch SM (distinct id; multi-frame) |
| `/pp-browser/circuit/1.0.0` | **circuit** | Extend with v2 ops; do not mint a sibling tunnel family |
| `/pp-browser/circuit-carrier/1.0.0` | *(plumbing)* | Product-owned wire id; configured into Amp `EnableNestedCarrierAccept` (Amp library default is `/amp/circuit-carrier/1.0.0`) |
| `/pp-browser/rpc/chat/1.0.0` | **rpc** | Live chat envelopes |
| `/pp-browser/rpc/history/1.0.0` | **rpc** | Peer history request/response |
| `/pp-browser/blob/1.0.0` | **blob** | Content-addressed attachment bytes |
| `/pp-browser/realtime/1.0.0` | **realtime** (E2E) | Call-media bundle |
| `/pp-browser/datagram-relay/1.0.0` | **realtime** (blind hop) | Content-agnostic fan-out ([N021](../../projects/p2p-mesh/DECISIONS.md#n021--generic-media_relay-framing-qos-channel-types)) |


## Use-case coverage (composition)

| Use case | Composition |
|----------|-------------|
| Chat / receipts / presence / call signaling | **rpc** envelopes |
| History / contact paste / small config pull | **rpc** |
| Photos, docs, video files, icons | **blob** |
| 1:1 / group A/V | **realtime** E2E (`call-media`) |
| SFU / media hop / live opaque fan-out | **realtime** blind hop (`media-relay`) |
| NAT traversal | **reach**, then direct; else **circuit** |
| Call over NAT | **circuit** → carrier → nested Session → **realtime** E2E ([A024](../../projects/adp/DECISIONS.md#a024--amp-call-media-over-circuit--nested-session)) |
| Phone book / PeerId routing | **discover** |
| SoftMigrate attach | Existing **realtime** hop control ops ([N026](../../projects/p2p-mesh/DECISIONS.md#n026--media_relay-per-stream-attach-state-machine)) |

Policy (who may hop, pricing, MeshHopPolicy), codecs, and UI stay **above** the wire kinds.

## Behavior matrix (as implemented)

Dimensions below describe **conversation behavior** on the OPEN, not delivery path (direct / punch / circuit / HTTP). Channel-class and policy details: [AMP-CHANNEL.md](AMP-CHANNEL.md).

### App / product conversations

| Dimension | `rpc/chat` | `rpc/history` | `blob` | `realtime` (call-media) | `datagram-relay` (hop) |
|-----------|------------|---------------|--------|-------------------------|-------------------------|
| **Wire id** | `/pp-browser/rpc/chat/1.0.0` | `/pp-browser/rpc/history/1.0.0` | `/pp-browser/blob/1.0.0` | `/pp-browser/realtime/1.0.0` | `/pp-browser/datagram-relay/1.0.0` |
| **Kind** | rpc | rpc | blob | realtime (E2E) | realtime (blind hop) |
| **L3 class** | Transactional | Transactional | Bulk | RealtimeControl + Realtime | RealtimeControl + Realtime |
| **QoS** | Reliable | Reliable | Reliable | control Reliable; media BestEffort | hop control Reliable; media BestEffort |
| **Duplex** | half (`read_once`-style ack) | half (req → resp) | half (meta ↔ bytes/error) | full (session) | full (session / fan-out) |
| **Send size** | small JSON envelope | small JSON request | **large** ciphertext | tiny control + continuous media | opaque datagrams |
| **Send pattern** | push event | pull query | fetch or push by hash | continuous while call up | continuous / fan-out |
| **Delivery guarantee** | reliable (AMP); app may retry | reliable query; paginate on fail | reliable transfer or fail | media **lossy OK**; control reliable | media lossy OK; hop may drop |
| **Latest-wins / drop** | no (do not drop chat) | no | no (`ChatBlob` drop never) | media: **drop oldest** (e.g. queue 64) | media: aggressive drop (queue ~2) |
| **Broadcast / fan-out** | 1:1 peer (group = many 1:1 or later relay role) | 1:1 pull | 1:1 (CDN secondary) | 1:1 E2E | **multi-receiver hop** |
| **Return size** | tiny ack `{"ok":true}` | **medium** message batch | large bytes or error JSON | hello_ack + ongoing media | hop control acks + media |
| **Return order** | ack only | batch ordered by seq/cursor | stream/object completeness | media not strictly app-ordered | hop does not reorder for meaning |
| **Latency sensitivity** | moderate (chat) | low–moderate (sync) | low (bulk) | **high** (media) | **high** |
| **Lifetime** | short OPEN | short OPEN (longer read timeout) | short OPEN | **long-lived call SM** | **long-lived hop / SoftMigrate** |
| **In-band “signal”** | envelope `op`s (call invite, attachment pointer, …) | history request fields | blob `op` fetch/push | bundle hello / channel admit | attach / quote / SoftMigrate ops |
| **Code owner (approx.)** | `AmpDirectChatService` | `AmpChatHistoryService` | `AmpChatBlobService` | `CallMediaLegCoordinator` | `AmpMediaRelayCoordinator` |

**Mental model:** `rpc/chat` is the small **send + tiny return** bus (product signals via envelope `op`). `rpc/history` is the same reliability class with **pull + larger return**. `blob` is **large reliable** transfer. `realtime` / `datagram-relay` are **time-sensitive**, lossy media allowed, drop-oldest, long-lived.

### Infra conversations (path / discovery — not app payload)

| Dimension | `reach` dial-back | `reach/punch` | `circuit` | `circuit-carrier` | ch0 identify | `directory` / `dht` |
|-----------|-------------------|---------------|-----------|-------------------|--------------|---------------------|
| **Wire id** | `/pp-browser/reach/1.0.0` | `/pp-browser/reach/punch/1.0.0` | `/pp-browser/circuit/1.0.0` | `/pp-browser/circuit-carrier/1.0.0` | (ch0; no product id) | `/pp-mesh/directory/1.0.0`, `/pp-mesh/dht/1.0.0` |
| **Kind** | reach | reach | circuit | plumbing | identify | discover |
| **Send size** | small JSON | small multi-frame JSON | control JSON + opaque splice | opaque nested Session | caps JSON | small control JSON |
| **Guarantee** | reliable short probe | reliable SM; may fail → circuit | reliable setup; then opaque forward | large BestEffort-ish carrier queue for bursts | reliable | reliable |
| **Return** | ok / dialed / error | sync / result frames | bridge / tunnel up | nested traffic | peer caps | list / find results |
| **Latency** | setup-sensitive | setup-sensitive | setup then follows target | follows nested realtime/rpc | once per session | moderate |
| **Lifetime** | one-shot | short multi-frame SM | live tunnel while needed | while nested call/path up | session start | short ops |
| **Role** | “can you dial me?” | NAT assist | path when direct fails | nest E2E Session in tunnel | “what do you speak?” | find peers / names |

**Delivery path** (direct ADP, punch-assisted direct, circuit → nested Session, HTTP/CDN fallback) is **orthogonal** to the rows above: same `protocol_id` conversation, different way to reach the peer.

### Where facilitation / “signal” lives

| Need | Home | Why |
|------|------|-----|
| Call invite / accept / hangup; “attachment available” | **`rpc/chat`** envelope `op` (or later `rpc/call` if policy diverges) | Short product control; same inbox/thread story |
| History catch-up | **`rpc/history`** | Pull API; separate handler / timeouts |
| Blob fetch / push / transfer error | **In-band on `blob`** | Steers the bulk conversation |
| Call-media hello / channel admit; SoftMigrate attach | **In-band on `realtime` / hop** | Steers the live session |
| Path setup | **`reach` / `circuit` / carrier** | Delivery, not app payload |

## Related

- [NETWORKING.md](../architecture/NETWORKING.md) — HTTP + Amp doctrine  
- [AMP-CHANNEL.md](AMP-CHANNEL.md) — L3 classes and OPEN `protocol_id`  
- [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) — envelope / history payloads  
- [RELAY_SCOPE.md](../../projects/p2p-mesh/RELAY_SCOPE.md) — relay roles (not new kinds)  
