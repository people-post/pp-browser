# L4 protocol kinds

**Tier:** contracts  
**Status:** Accepted design (2026-09-04) — [A028](../../projects/adp/DECISIONS.md#a028--l4-protocol-kinds--seven-conversation-shapes) · [N030](../../projects/p2p-mesh/DECISIONS.md#n030--adopt-l4-protocol-kinds-gate)  
**Stack:** [STACK.md](../../projects/adp/STACK.md) · L3 [AMP-CHANNEL.md](AMP-CHANNEL.md)

## Role

AMP L1–L3 already provide association, crypto, mux, QoS, and fragmentation. **L4 `protocol_id` strings name conversation shapes**, not product features. This document freezes the complete set of kinds so agents and humans do not mint a new `/pp-…/1.0.0` for every feature.

Wire `protocol_id` strings are **kind-aligned** (no dual advertise; product not released). Shipped ids: `/pp-browser/rpc|blob|realtime|datagram-relay|circuit|reach/1.0.0` plus `/pp-mesh/directory|dht/1.0.0`. `rpc` demuxes envelope vs history via `op`. `reach` uses `/reach/1.0.0` (dial-back) and `/reach/punch/1.0.0` (punch SM).

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
| `/pp-browser/circuit-carrier/1.0.0` | *(plumbing)* | Outer splice target for nested Session — not a product kind |
| `/pp-browser/rpc/1.0.0` | **rpc** | `op=envelope` live chat; `op=history` history sync |
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

## Related

- [NETWORKING.md](../architecture/NETWORKING.md) — HTTP + Amp doctrine  
- [AMP-CHANNEL.md](AMP-CHANNEL.md) — L3 classes and OPEN `protocol_id`  
- [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) — envelope / history payloads  
- [RELAY_SCOPE.md](../../projects/p2p-mesh/RELAY_SCOPE.md) — relay roles (not new kinds)  
