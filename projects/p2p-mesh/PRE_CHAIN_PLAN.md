# Pre-chain engineering plan (N029 Phases A–C)

**Status:** Active plan  
**Date:** 2026-09-02  
**Branch intent:** land before blockchain / on-chain names (N029 Phase D)  
**North star:** [NAME_DIRECTORY_NORTH_STAR.md](NAME_DIRECTORY_NORTH_STAR.md) · ADR [N029](DECISIONS.md#n029--name-directory-north-star-chain-later-http-now)  
**Related:** [MESH_DIRECTORY.md](MESH_DIRECTORY.md), [DISCOVERY_ROADMAP.md](DISCOVERY_ROADMAP.md)

This plan is **ordering + gaps only**. It does **not** implement chain RPC, on-chain registration, or terminal `pp-beacon` embedding.

## Goal before blockchain era

Ship a product where users **find people and mesh services by Account ID / handle**, dial via PeerId under the hood, and pp-node remains the **uniform edge** — without teaching PeerIds or painting a corner for later chain names.

```text
Phase A  harden HTTP phone book + stable port + frozen fields   ← first release
Phase B  optional Amp directory twin (same records)
Phase C  ledger_gateway capability + hop hooks (no chain runtime yet)
Phase D  on-chain names  ← OUT OF SCOPE here
```

## Snapshot (2026-09-02)

| Area | State |
|------|-------|
| L0 pinned seed (N002) | Done |
| N027 HTTP `ListMeshNodes` / person lookup / pp-node publish | Done |
| `MeshDirectoryCache` + hop `DirectoryNode` (n-dir) | Done (see DISCOVERY_ROADMAP acceptance) |
| DHT PeerId → multiaddr only (N028) | Done through n2-hard |
| Abstract `INameDirectory` seam | **Landed** (nd1) |
| Frozen record fields on cache rows | **Landed** (nd2) |
| `directory.providers[]` pluggable list | **Landed** (nd3) |
| Amp directory channel (Phase B) | **Landed** (nd4) — `/pp-mesh/directory/1.0.0` |
| `ledger_gateway` capability (Phase C) | **Landed** (nd5) — vocab + hop collector |
| Unpinned DNS dial | Correctly **not** shipped |

Stale note: DISCOVERY_ROADMAP “Current state” table still said n-dir unwired; acceptance checkboxes and code are ahead — treat this plan as source of truth for **remaining** pre-chain work.

---

## Work packages

### nd0 — Doc / tracker hygiene (this PR family)

- [x] N029 + NAME_DIRECTORY_NORTH_STAR
- [x] Keep DISCOVERY_ROADMAP n-dir “current state” table honest (done / remaining)
- [x] PHASES.md: add **nd** track pointing here; do not reopen finished n-dir/n2 checklists
- [ ] Mark MESH_DIRECTORY Phase E smoke items still open vs done

**Exit:** Engineers open this file first for “what’s left before chain.”

---

### nd1 — Freeze the name-directory port (Phase A blocker for clean Phase D)

**Why first:** Without a seam, HTTP JSON and hop policy stay coupled; chain swap becomes a rewrite.

**Deliver**

1. Introduce `INameDirectory` (or clearly document `IDirectoryClient` as the port and narrow it):
   - `Resolve(name)` → normalized `NameRecord`
   - `ListService(kind)` → `NameRecord[]` (`mesh_node` now; kinds stay stringly/extensible)
   - `PublishSelf` stays on registration client **or** is a method on the same port with a clear split — pick one and document
2. Define `NameRecord` matching N029 frozen fields:

```text
name / account_id
peer_id
endpoints[]          // multiaddr hints
entity_kind
capabilities{}       // circuit_relay, media_relay, dht, (+ reserved ledger_gateway)
seq                  // optional until www emits it; default 0
expires_at
```

3. Map existing types:
   - `DirectoryHit` / `MeshNodeHit` → `NameRecord`
   - `MeshDirectoryNode` → either become `NameRecord` or carry the missing fields through the cache
4. `HttpDirectoryClient` implements the port; `MessagingHub` / hop builders consume **only** `NameRecord` / port snapshots
5. Unit tests: mapping fixtures; mock directory behind the port

**Touchpoints:** `src/base/net/ServiceClients*`, `src/base/data/Config.h` (`MeshDirectoryNode`), `src/base/mesh/discovery/*`, `src/feature/messaging/MessagingHub.cpp`, tests

**Exit:** No hop/UI code parses Brief-only JSON keys outside the HTTP adapter.

**Anti-scope:** no Amp protocol, no chain, no DNS dial.

**Status:** Landed — `INameDirectory` + `DirectoryClientNameDirectory` + `NameRecord` in `src/base/mesh/discovery/NameDirectory.*`; MessagingHub cache fetcher uses `ListService("mesh_node")`.

---

### nd2 — Complete Phase A phone-book UX & data fidelity

**Deliver**

1. **Preserve identity fields in cache** — do not drop `account_id` / nickname / `expires_at` when flattening `MeshNodeHit` → hop rows (needed for “resolve by name” and later chain claim)
2. **Person resolve path** — ensure Add Contact / sync-from-directory uses `LookupByAccount` / search and stores **Account ID** as the stable key; PeerId + multiaddrs as cache (align with multi-device Account ID work)
3. **seq** — if www does not yet return `seq`, add optional field + client default `0`; document server follow-up (www) without blocking client
4. **Capabilities vocabulary** — keep bool map extensible (`dht` already on mesh config; directory ads may grow); avoid hard-coded two-bool-only structs in the port layer
5. **Smoke** — pp-node publishes → browser cache lists node → circuit/media can pick it; person lookup by account without PeerId paste

**Exit:** First-release user story works: join via defaults, find org node / person by handle, dial without typing PeerId.

**Status:** Landed for cache/parse fidelity (`MeshDirectoryNode` + `MeshNodeHit`/`DirectoryHit` fields; optional `seq`/`entity_kind`/extended caps). Manual Phase E smoke still open. Person Account-ID UX largely pre-existing.

---

### nd3 — Provider config hygiene (still Phase A)

**Deliver**

1. Config: prefer `directory.providers[]` ordered list **or** keep `base_url` as provider[0] with explicit `transport: http` (N027 already sketched)
2. Cache fetcher iterates providers with failover (empty list → platform default Brief URL)
3. Docs: CONFIGURATION.md — “directory provider (HTTP now; Amp/chain later)”

**Exit:** Swapping the phone-book backend is a provider entry, not a MessagingHub rewrite.

**Status:** Landed — `DirectoryConfig.providers[]`, `EffectiveDirectoryProviders`, `FailoverDirectoryClient`, factory HTTP wiring; Amp backends owned by MeshHost/MessagingHub (nd4). Settings UI edits `base_url` only (clears providers).

---

### nd4 — Amp directory twin (Phase B)

**Deliver**

1. Spec: `docs/contracts/` or project ADR slice — channel id e.g. `/pp-mesh/directory/1.0.0`, request/response mirroring `ListMeshNodes` / `NameRecord[]` (signed blob optional v1)
2. pp-node (and/or org seed): serve directory answers from the same data it would HTTP-publish (or proxy to local cache of registrations — **not** an open HTTP CONNECT proxy)
3. Client: after L0 MSH to seed (or preferred introducer), try Amp directory; fall back to HTTP
4. Wire as second `INameDirectory` / provider `transport: amp`
5. Tests: link harness two-node directory query; failure falls back to HTTP

**Exit:** Mesh-native phone book path exists; HTTP remains twin, not sole door.

**Anti-scope:** fog anycast, root-signed epoch manifests, invite sponsorship (theory only until scheduled).

**Status:** Landed — `docs/contracts/MESH_DIRECTORY_AMP.md`; `AmpDirectoryService` + `AmpDirectoryClient`; MeshHost advertise/serve; MessagingHub Amp-first `MeshDirectoryCache` fetcher then HTTP; pp-node hosts snapshot; `CreateServiceClients` still HTTP-only (Amp needs PeerLinkManager).

---

### nd5 — `ledger_gateway` capability prep (Phase C, no chain runtime)

**Deliver**

1. Add `ledger_gateway` to:
   - `MeshCapabilities` / `MeshCapabilitiesAd`
   - Config + Me → Network (Node / pp-node only) **only when** a stub or real gateway path exists (N008 — no inert checkbox); until then: config/env + docs only is OK
2. Directory / Amp records may advertise the capability
3. Hop policy: filter/collect candidates with `ledger_gateway` for a future `CollectLedgerGatewayCandidates` (can be unused by product until ledger transport lands)
4. Align naming with [platform-integration](../../../pp-ledger/docs/platform-integration.md): opaque upstream, terminal beacon private
5. Explicit non-goals: embedding `pp-beacon`, ledger RPC dial, settle UI

**Exit:** Capability bit and hop hook exist so chain enablement is “turn on transport + resolver backend,” not “redesign mesh_node.”

**Status:** Landed — capability vocab + `CollectLedgerGatewayHopCandidates` (unused by dial paths until ledger transport). UI checkbox still deferred (N008).

---

## Suggested PR sequence

| PR | Package | Depends on |
|----|---------|------------|
| 1 | nd0 tracker hygiene | — |
| 2 | nd1 `INameDirectory` + `NameRecord` | nd0 |
| 3 | nd2 field fidelity + UX smoke | nd1 |
| 4 | nd3 providers[] | nd1 |
| 5 | nd4 Amp directory spec + seed serve + client | nd1–nd2 |
| 6 | nd5 `ledger_gateway` vocab + hop stub | nd1 |

Parallel: www optional `seq` on mesh_node docs can land anytime after nd1 field reservation.

## Explicitly deferred (Phase D+)

- On-chain name registration / PeerId rotation txs
- Chain as `INameDirectory` backend
- Mesh DHT storing names
- Unpinned `/dns4` dial
- Terminal ledger Beacon as public bootstrap
- Fog rendezvous / epoch root keys (optional later L0 hardening)

## Anti-blockers (repeat from N029)

1. No hop/UI hard-wire to Brief URL parsing outside the HTTP adapter  
2. DHT / contacts are not the name registry  
3. No public terminal `pp-beacon` door  
4. Account ID / handle remains the human key (no second display-name system)  
5. Document providers as interim; chain becomes authority later  
6. PeerId is the stable mesh core; multiaddrs are hints  
7. No unpinned DNS L0  
8. Keep `entity_kind` + capabilities 1:1-mappable to future chain rows  

## First-release “good enough” bar

Ship when **nd1 + nd2** are done (nd3 strongly preferred).  
**nd4 / nd5** can trail first release if needed, but nd1 must not slip — it is the load-bearing seam for blockchain-era swap.
