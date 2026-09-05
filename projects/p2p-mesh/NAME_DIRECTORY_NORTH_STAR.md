# Name directory north star — phone book, edge router, phased path

**Status:** Accepted design (N029)  
**Date:** 2026-09-02  
**ADR:** [N029](DECISIONS.md#n029--name-directory-north-star-chain-later-http-now)  
**Related:** [MESH_DIRECTORY.md](MESH_DIRECTORY.md) (N027), [DISCOVERY_ROADMAP.md](DISCOVERY_ROADMAP.md), [MESH_DHT.md](../../docs/contracts/MESH_DHT.md) (N028), [platform-integration](../../../pp-ledger/docs/platform-integration.md)

## North star (one paragraph)

**pp-node is the uniform edge router** (circuit, media, and later ledger RPC on the same hop machinery). **The chain is the eventual phone book** (human name → PeerId[+endpoint hints]). **Amp is the wire.** **L0 bootstrap only finds a door to the phone book.** HTTP Brief directory and mesh DHT are **caches / reachability**, not final name authority.

User-facing promise across all phases: **find peers and services by name; never require users to type PeerIds.**

## Trust vs reachability

| Layer | Job | Examples |
|-------|-----|----------|
| **Human name** | “Which person / org / service?” | Account ID, org handle, later on-chain unique name |
| **Phone book (truth)** | Name → PeerId[+hints], kind, capabilities, seq | HTTP directory now; **chain registry later** |
| **Reachability** | PeerId → dialable multiaddrs | DHT `peer_routing`, contacts, advertise, mDNS |
| **L0 door** | First packet when cold | N002 pinned seed; optional fog / dnsaddr **catalog** of pinned beacons |
| **Edge** | Route the right Amp channel | pp-node: circuit / media / **ledger_gateway** |

Do **not** conflate:

- **Mesh epoch / manifest / attestation** (signed bootstrap catalog) with **pp-ledger `pp-beacon`** (terminal chain authority). Same metaphor, different roles — prefer “manifest” / “name record” in mesh docs.
- **Unpinned DNS dial** (`/dns4/…` without `/p2p/…`) with a memorable name. DNS may distribute **pinned** locator lists; it must not replace PeerId pinning for L0 trust.

## Joint model with pp-ledger

```text
brief.global / L0 seed / optional fog
        │
        ├─► Comms phone book  →  mesh_nodes (circuit, media, DHT)
        │
        └─► (later) Ledger gateways →  pp-node ledger_gateway
                                            │
                                            └─► (private) terminal pp-beacon
```

- Terminal **ledger Beacon** stays scarce / often private ([platform-integration](../../../pp-ledger/docs/platform-integration.md)).
- Public edge is **pp-node** with capabilities — including future **`ledger_gateway`** (opaque upstream), same as circuit/media.
- Fleet discovery for chain nodes stays **curated** (not mesh DHT). Mesh DHT remains **PeerId → multiaddr** only (N028).
- On-chain **name registry** (final phase): users register / rotate PeerIds under easy unique names; chain is name truth; DHT/HTTP stay helpers.

## Stable seam (build now — keep forever)

Product and hop policy talk only to an abstract **name directory** port (evolve `IDirectoryClient` or introduce `INameDirectory`):

```text
Resolve(name) → { peer_id, endpoints[], entity_kind, capabilities, seq, expires }
ListService(kind) → [same records]     // mesh_node; later ledger_gateway via caps
PublishSelf(record) → ack              // person / mesh_node renew
```

| Phase | Backend behind the port |
|-------|-------------------------|
| **A — first release** | Brief HTTP (N027) |
| **B — native door** | + Amp directory channel mirroring the same records (optional) |
| **C — ledger on edge** | + `ledger_gateway` capability on mesh_node / hop policy (chain RPC routing; names still HTTP/Amp) |
| **D — chain names** | Chain registry = source of truth; HTTP/Amp = indexers/caches |

UI, `MeshHopPolicy`, contacts, and bootstrap **must not** hard-wire Brief URL JSON shapes or chain tx types.

### Frozen record fields

```text
name / account_id     // human key (v1 = Brief account; later = chain name)
peer_id               // mesh identity
endpoints[]           // dial hints (optional long-term; PeerId is the stable core)
entity_kind           // person | mesh_node | …
capabilities{}        // circuit_relay, media_relay, dht, ledger_gateway, …
seq                   // monotonic updates
expires_at            // lease (HTTP now; chain lease/rent later)
```

Prefer authoritative **name → PeerId**; treat multiaddrs as hints so chain-era records need not churn on every IP change.

## Phased path (chain not in first release)

### Phase A — First release (no chain)

**User need:** join mesh, find people/org nodes, dial, calls/relay — without PeerIds.

| Ship | Final-shaped reason |
|------|---------------------|
| L0 pinned seed ([N002](DECISIONS.md#n002--seed-multiaddr-ip--443--peerid-no-dns)) | Tiny cold door only |
| N027 HTTP directory as **v1 phone book** | Handle → endpoints |
| `entity_kind` + capabilities vocabulary | Same fields later on chain |
| n-dir: directory → hop policy | Services from phone book |
| DHT = PeerId → multiaddr only ([N028](DECISIONS.md#n028--amp-native-mesh-dht-find_peer-v1)) | Never name authority |
| pp-node advertise/renew as `mesh_node` | Same record later anchored on chain |
| Contacts pin **Account ID / handle**; PeerId is cache | Matches final UX |

### Phase B — Native door (still no chain)

Optional Amp `/pp-mesh/directory` (or equivalent) on seed/introducer: **same records** as HTTP. Prefer Amp then HTTP. Prepares mesh-native resolve without chain.

Introducer answers **directory queries** (or serves a signed/cached blob). It is not an open HTTP proxy and not an unpinned trust root.

### Phase C — Ledger capability on pp-node

Add **`ledger_gateway`** to the capability vocabulary and hop picker. pp-node routes ledger RPC like calls (opaque upstream to terminal beacon). Phone book backend still HTTP/Amp.

### Phase D — On-chain names

Swap resolver backend: chain becomes truth; HTTP becomes projector/cache. Migrate: claim/link existing Account ID → chain name; PeerId rotation = chain updates; keep `seq` monotonic across worlds.

## Anti-blockers

1. Do not hard-wire hop pick / UI to `directory.base_url` parsing — only the name-directory port.
2. Do not make DHT or the local contact book the **name** registry.
3. Do not put terminal `pp-beacon` on the public bootstrap path — only future `ledger_gateway` nodes.
4. Do not invent a second display-name system divergent from Account ID / handle — v1 handle is the chain-name candidate.
5. Do not document “HTTP renew = eternal source of truth” — say “directory provider (HTTP now, chain later).”
6. Do not require multiaddrs on the authoritative record forever — PeerId is the stable core.
7. Do not ship unpinned DNS dial as L0.
8. Do not fork people-search / mesh_node schema away from `entity_kind` + capabilities — chain rows should map 1:1.

## Optional theory (not required for Phase A)

- **Fog rendezvous:** untrusted lobby that only hints at (or passes through) sealed directory/manifest data; authority remains phone book + L0 pins.
- **Root-signed epoch manifests:** rotate seed PeerIds/locs without app-store updates; fog/DNS distribute manifests; app ships root keys.
- **Invite sponsorship:** social L0 via contact invite; org seed remains backup door.

These may inform Phase B+; they must not delay Phase A’s HTTP phone book + n-dir.

## UX by era

| Era | User remembers | App does |
|-----|----------------|----------|
| A | nothing / Brief account | HTTP phone book + L0 seed |
| B | nothing | Amp directory twin |
| C | nothing / account | + ledger RPC via pp-node gateways |
| D | unique on-chain name | Resolve on chain via any `ledger_gateway` |

## Near-term engineering order

See **[PRE_CHAIN_PLAN.md](PRE_CHAIN_PLAN.md)** for the full pre-blockchain work package list (nd0–nd5).

Summary:

1. Freeze name-directory port + record schema (nd1).  
2. Field fidelity + first-release UX smoke (nd2); providers[] (nd3).  
3. Keep L0 seed minimal; no DNS-unpinned dial.  
4. Later: Amp directory mirror (nd4) → `ledger_gateway` capability prep (nd5) → chain provider (Phase D, separate plan).
