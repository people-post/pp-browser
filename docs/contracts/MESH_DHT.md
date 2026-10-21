# Mesh DHT — AMP Kademlia peer routing (spec)

**Tier:** contract (draft — **n2-spec**; implementation **n2-core**)  
**Status:** Draft v1 (routing table, iterative lookup, publish at the k closest: 2026-10-21)  
**Date:** 2026-09-02, updated 2026-10-21 — design: pp-cpp-mesh [docs/DHT.md](https://github.com/people-post/pp-cpp-mesh/blob/develop/docs/DHT.md)  
**Related:** [N028](../../projects/p2p-mesh/DECISIONS.md#n028--amp-native-mesh-dht-find_peer-v1), [DISCOVERY_ROADMAP](../../projects/p2p-mesh/DISCOVERY_ROADMAP.md), [SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md), [AMP-CHANNEL.md](AMP-CHANNEL.md), [platform-integration](../../../pp-ledger/docs/architecture/PLATFORM_INTEGRATION.md) (pp-ledger fleet DHT retired)

## Purpose

Decentralized **PeerId → multiaddr** lookup for pp-browser mesh when contacts and HTTP directory are insufficient. This is **not** the BitTorrent DHT removed from pp-ledger standalone fleet nodes; ledger backbone continues to use **curated ADP multiaddrs**.

| Plane | Discovery |
|-------|-----------|
| pp-ledger fleet (beacon/relay/miner) | Curated config multiaddrs (AMP) |
| pp-browser / pp-node mesh | Contacts → directory → org seed → **optional DHT** |

DHT **feeds candidates** only. [`MeshHopPolicy`](../../src/base/people/MeshHopPolicy.h) and relay scope (N020/N023) still gate dial and hop selection.

## Participation rules

| Rule | Value |
|------|--------|
| Role | Desktop **Node** only (`Node && capabilities.dht`) |
| Mobile / Client | Never participates (consume-only) |
| Default | `capabilities.dht = false` |
| Publish gate | Reachability ≠ `Blocked`; advertise dialable ADP multiaddrs only |
| Bootstrap | `mesh.bootstrap_peers` ∪ cached [`MeshDirectoryCache`](https://github.com/people-post/pp-cpp-mesh/blob/develop/include/mesh/discovery/MeshDirectoryCache.h) nodes (n-dir) |
| Lookups | A participating Node walks the DHT (iterative, below). A Client asks its query peers only and follows no `closer_peers` — a walk tells strangers whom you look for |

## Transport

DHT RPC runs on AMP **control** channels:

| Field | Value |
|-------|--------|
| `protocol_id` | `/pp-mesh/dht/1.0.0` |
| Channel class | Control (reliable, half-duplex request/response) |
| Framing | One JSON object per DATA frame (UTF-8) |
| Crypto | Outer AMP Session AEAD (same as other mesh control) |

No second UDP stack; reuse existing Amp association to peers.

## Kademlia parameters (v1)

| Constant | Value | Notes |
|----------|-------|-------|
| `k` | 20 | Bucket size target (`mesh.dht.k_bucket_size` override reserved) |
| `α` | 3 | Parallel lookup width |
| `k_closer` | 20 (= `k`) | Most `closer_peers` in one answer |
| Key width | 256 bit | XOR metric on `Key256(record_key)` |
| Record key (v1) | `BLAKE2b-256("pp-mesh/dht/peer-routing/v1\\0" ‖ PeerId_multihash_bytes)` | Stable per target PeerId |

## Wire messages

All messages include:

| Field | Type | Required |
|-------|------|----------|
| `op` | string | yes |
| `req_id` | string (uuid) | yes |
| `version` | int | yes (= `1`) |

### `ping`

Request: `{ "op": "ping", "req_id", "version": 1 }`  
Response: `{ "op": "pong", "req_id", "version": 1, "peer_id": "<local PeerId base58>", "participant": true }`

`participant` (in `pong` and `find_peer_result`): the responder takes part in the DHT. Absent =
`false` (older nodes).

### `find_peer` (v1 consumer)

Request:

```json
{
  "op": "find_peer",
  "req_id": "…",
  "version": 1,
  "peer_id": "12D3KooW…"
}
```

Response (success):

```json
{
  "op": "find_peer_result",
  "req_id": "…",
  "version": 1,
  "peer_id": "12D3KooW…",
  "records": [ { …PeerRoutingRecord… } ],
  "closer_peers": [ { "peer_id": "…", "multiaddr": "/ip4/…/adp/…/p2p/…" } ],
  "participant": true
}
```

| Field | Semantics |
|-------|-----------|
| `records` | Zero or more signed `peer_routing` records matching `peer_id` (best `seq` first) |
| `closer_peers` | Up to `k_closer` contacts from the responder's routing table closest to the lookup key, never the asker. Each `multiaddr` ends in `/p2p/<peer_id>` (dialling it authenticates the peer); entries that do not are skipped. A hint to ask — never a routing-table entry by itself |

Response (error):

```json
{ "op": "error", "req_id": "…", "version": 1, "code": "not_found", "message": "…" }
```

### `store` (v1 provider — self record only)

Request:

```json
{
  "op": "store",
  "req_id": "…",
  "version": 1,
  "record": { …PeerRoutingRecord… }
}
```

Response: `{ "op": "store_result", "req_id", "version": 1, "ok": true }`

**v1 implementation scope:** nodes STORE only their **own** `peer_id` record; relay STORE for third parties deferred (n2-hard).

**Where:** a Node STOREs its record at the (up to) `k` nodes closest to its own key that answer a
walk to it, and at its query peers (where Clients ask). It re-publishes at `ttl_seconds / 2` and
at once when its addresses change.

## Routing table and lookup

- **Admission:** a peer enters the routing table only after an authenticated exchange that shows
  it takes part — its `store` was accepted, or it answered with `participant: true`. Its dial
  address comes from its own signed record, or is the hinted address an authenticated dial just
  reached. At most 2 entries per IPv4 /24 (IPv6 /48) per bucket; a full bucket keeps its
  long-lived entries (the oldest is pinged only after 15 min of silence).
- **Liveness** is the mesh reach record's: a peer it holds down (and with no link up) is not
  dialled, not stored at, and leaves the table.
- **Lookup:** iterative Kademlia — `α` asks in flight to the closest not yet asked, hints joined
  as they arrive; it stops asking at a valid record, once the `k` closest seen have answered or
  failed, or after 30 asks / 8 s. A self answer (the record's subject answering) beats any other;
  else the highest `seq`. A lookup answered without a record is not repeated for 60 s.

## `PeerRoutingRecord` (v1)

```json
{
  "type": "peer_routing",
  "version": 1,
  "peer_id": "12D3KooW…",
  "seq": 42,
  "ttl_seconds": 3600,
  "issued_at": 1735689600,
  "multiaddrs": [
    "/ip4/203.0.113.10/udp/443/adp/1.0.0/p2p/12D3KooW…"
  ],
  "signature_b64": "…",
  "signature_alg": "ml-dsa-65",
  "public_key_b64": "…"
}
```

`public_key_b64` is the signer's ML-DSA-65 public key. It is not part of the signing bytes (the
PeerId binds it). A publisher always sets it; a STORE server keeps the sender's session key there.

Validation (implementations MUST):

1. `signature_alg` = `ml-dsa-65`.
2. `peer_id` matches mesh identity derived from signing key: when the record's subject answers, the
   key is its authenticated session key; otherwise `public_key_b64` must derive `peer_id` and the
   signature must verify under it. A record from anyone else without `public_key_b64` is dropped.
3. Each `multiaddr` contains `/p2p/<peer_id>` consistent with `peer_id`.
4. Prefer ADP multiaddrs (`/adp/1.0.0/`) for dial; ignore undialable private addrs unless link-local policy applies.
5. Reject when `issued_at + ttl_seconds` ≤ now (relay clock skew: allow 60s grace).
6. Accept only if `seq` ≥ last seen seq for `(peer_id, record_key)`.

### Capabilities (n2-caps)

Optional signed object using the same vocabulary as Brief directory `mesh_node`:

```json
"capabilities": {
  "circuit_relay": true,
  "media_relay": true
}
```

When present, capabilities are included in the signing bytes (after multiaddrs). Consumers map them into `MeshHopPolicy` / `FilterHopsByMediaRelayAds` the same way as directory nodes (`MeshHopAffinity::DhtDiscovered`).

### Signing bytes

Domain-separated canonical payload (before ML-DSA-65 sign):

```text
pp-mesh:dht-record-v1\0
sign_version=1
type=peer_routing
peer_id=<utf8 peer_id>
seq=<i64 be>
ttl_seconds=<i64 be>
issued_at=<i64 be>
multiaddrs=<count u32 be> then each len-prefixed utf8 multiaddr
capabilities_present=<u8 0|1>
[when present: circuit_relay=<u8 0|1>, media_relay=<u8 0|1>]
```

(`signature_b64` is standard raw signature encoding used elsewhere in pp-browser.)

## Consumer integration (n2-core / n2-caps)

| Step | Behavior |
|------|----------|
| Lookup trigger | Optional prefetch when contact has PeerId but no fresh endpoint; partition escape after directory + seed fail |
| Merge | `FindPeer` results → `RegisterPeerDirectEndpoint` |
| Hop policy | Map to `MeshHopAffinity::DhtDiscovered`; rank below directory/seed; subject to `MeshHopPolicy` / `FilterHopsByMediaRelayAds` |
| Caps (n2-caps) | Signed `capabilities` on `peer_routing`; same fields as directory `mesh_node` |
| Negative cache | Short TTL on failed lookups; never block UI thread (worker + callback) |

Capability records (`circuit_relay`, `media_relay`) ship in signed `peer_routing.capabilities` (**n2-caps**).

## Config (schema stub)

See [CONFIGURATION.md](../ops/CONFIGURATION.md#mesh-dht-n2). Implementation reads:

- `mesh.capabilities.dht` (bool, default `false`)
- `mesh.dht.record_ttl_seconds`, `find_peer_timeout_ms`, `max_concurrent_lookups`, `k_bucket_size`
- `mesh.dht.inbound_ops_per_peer_per_window`, `inbound_rate_window_seconds` (n2-hard)
- `mesh.dht.soft_reputation_penalty_threshold`, `soft_reputation_cooldown_seconds` (n2-hard)

UI checkbox ships with **n2-core** (N008 — no inert controls). Ops: `pp-node --status` / `/status` include `dht` + `dht_stats`.

## Hardening (n2-hard)

| Control | Behavior |
|---------|----------|
| Inbound rate limit | Per remote PeerId sliding window on FIND_PEER + STORE |
| STORE validation | Reject `not_self`, `expired`, `bad_signature`, `bad_key` (a `public_key_b64` that is not the sender's), `seq_regression` with typed `error` responses |
| Soft reputation | Skip peers that return malformed or wrongly signed FIND_PEER records (cooldown); an expired record is skipped, not held against its server |
| Concurrency | Cap outbound FIND_PEER by `max_concurrent_lookups` |

## Non-goals (v1)

- Content / blob routing
- Pubsub
- Open public relay market via DHT
- Shared DHT with pp-ledger BitTorrent `DhtRunner`
- libp2p Kademlia import (fork retired)
- Third-party STORE (relay storage for others)

## Versioning

| Axis | v1 |
|------|-----|
| Wire `version` | `1` |
| Record `type` | `peer_routing` only |
| `protocol_id` | `/pp-mesh/dht/1.0.0` |

Additive v2 may introduce `capability_routing` records without breaking v1 FIND_PEER.
