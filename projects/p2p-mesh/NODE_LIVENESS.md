# Node liveness — plan

**Status:** Plan (design accepted as [N031](DECISIONS.md#n031--node-liveness-short-lived-signed-records-observed-by-the-mesh); not started)  
**Date:** 2026-10-31  
**Related:** [privacy P011](../privacy/DECISIONS.md#p011--a-nodes-uptime-is-not-public-by-role), [N027](DECISIONS.md#n027--mesh-directory-entity_kind-pluggable-providers-bootstrapdirectory), [N029](DECISIONS.md#n029--name-directory-north-star-chain-later-http-now), [MESH_DIRECTORY.md](MESH_DIRECTORY.md), [MESH_DIRECTORY_AMP.md](../../docs/contracts/MESH_DIRECTORY_AMP.md), [ACCOUNT_CARD.md](../../docs/contracts/ACCOUNT_CARD.md)

## Problem

Clients need to skip nodes that are gone. Today a `mesh_node` registration lasts 90 days, so a stopped node stays listed for months. The heartbeat (PR #428) fixes that by reporting liveness **to the Brief directory**: every 5 minutes, signed, from the node's own network. That makes one server the authority on which nodes are alive, gives it each node's uptime and egress address, and drops a healthy node that cannot reach it. P011 limits the damage (opt-in for org seeds, no exact times in the listing, no IP in logs, stale cut-off only for heartbeat nodes). This plan replaces the reporting.

## Direction

Liveness is a property of a **record**, not of a server's memory, and is **observed** by whoever uses a node, not reported to one place.

1. **Signed short-lived node records.** A node signs its own description — the same pattern as account cards (self-proving, verified by the reader).
2. **Records spread through the mesh.** Nodes that serve the Amp directory (`/pp-mesh/directory/1.0.0`) keep each other's fresh records; any directory — Brief's HTTP one included — reads records instead of receiving pings.
3. **Clients judge by record freshness and their own dial results.** A fresh, valid record means "offered now"; a node the client reached recently is alive whatever any directory says.

## Phases

| Phase | What | Where |
|-------|------|-------|
| **L1 — Node record** | `NodeRecord`: `account_id`, account signing key, `peer_id`, `multiaddrs`, capabilities, `state` (`active` / `draining` / `retiring`), `issued_at`, `expires_at` (about 1 h after `issued_at`), ML-DSA-65 signature by the account key under its own domain (`pp-mesh:node-record-v1`). Codec + verify in pp-cpp-mesh `discovery/` beside `MeshNodeHitCodec`. Contract: `docs/contracts/NODE_RECORD.md` | pp-cpp-mesh, pp-browser docs |
| **L2 — Serve and carry** | pp-node re-signs every ~20 min and on a state change. The Amp directory serves `self + cached records`; directory nodes exchange fresh records with the directory peers they already hold links to (no new connections, nothing beyond what those peers already see). A record is dropped at `expires_at` | pp-node, pp-cpp-mesh `l4/` directory |
| **L3 — HTTP directory as a reader** | `GET /v1/mesh/nodes` carries the signed record verbatim (`record`), so a client verifies it and the directory cannot alter it. www learns records by reading them from a directory node over the mesh, or from the node's normal register/renew — not from a 5-minute ping | www, pp-cpp-mesh client |
| **L4 — Clients** | Rank and filter by record freshness plus local dial outcomes (the hop policy already skips failed hops); keep the last-good cache; never drop a node reached recently. A node whose record expired is tried last, not removed, until the client's own dials fail | pp-cpp-mesh, pp-browser |
| **L5 — Retire the heartbeat** | Once records flow, `POST /v1/mesh/heartbeat` and the `mesh_heartbeat` switch are removed; the listing's stale filter uses record expiry. Registration TTL for `mesh_node` can stay long: it is the account binding, not liveness | pp-node, www |

The record is meant to be the payload a chain name directory anchors later (N029): no field in it may depend on Brief.

## Open questions

- **Spam:** who may put a record into the directory nodes' caches? Proposal: only accounts with a live `mesh_node` registration (now) or chain name (later); caches cap per account.
- **Cadence vs churn:** 1 h validity and 20 min refresh are guesses; measure record traffic on the Amp directory before fixing them.
- **Volunteer nodes:** their records are public by role (P004) like their registration, but who learns their *refresh* timing is narrower than with the heartbeat (only the directory peers they already link with). Check that this is acceptable to volunteer operators before L2 ships.
- **Retiring:** a `retiring` record signed at shutdown is the clean goodbye; a crash leaves the record to expire (≤ 1 h).

## Not in scope

DHT (N028: PeerId → address only, never names or records); person directory entries (people are not listed).
