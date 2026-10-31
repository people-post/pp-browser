# Capability repos — design

**Goal:** pp-browser's capabilities — identity, the backend client, social (posts + content), messaging,
realtime (calls + broadcast), node — get boundaries clean enough that another app (pp-node on its own, a VPN
client, the ledger apps) can **pick** them, as apps already pick
[pp-cpp-amp](https://github.com/people-post/pp-cpp-amp) and [pp-cpp-mesh](https://github.com/people-post/pp-cpp-mesh).
pp-browser stays the client and integrator: UI, AI, app composition, and the version set it ships.

**How:** boundaries in the tree first, enforced by a gate (P0–P3). A capability leaves only when a second
app needs it (CR005), and then into **one capabilities repo** with a target set per capability, not a repo
each (CR006).

**Non-goals:** one engine target every app links whole; a repo per capability; splitting on a schedule.

## Where we are (measured 2026-10-30, `develop` at `0d77a6a35`)

**No second consumer yet.** The ledger apps pick pp-cpp-common, -crypto and -ui; pp-ledger picks pp-cpp-amp;
no app uses a pp-browser capability. pp-node, built from this tree, is the first that would.

### Capabilities in the tree

The gate's map (`scripts/check/check_capability_edges.py`, longest path prefix wins). kLOC without tests.

| Capability | Paths under `src/` | kLOC |
|------------|--------------------|------|
| base | `common` (the rest), `foundation` | 23.1 |
| people (identity) | `lib/people`, `domain/peers`, `common/directory` | 6.9 |
| net (backend client) | `lib/net`, `domain/registration` | 4.0 |
| social | `lib/posts`, `lib/content` | 3.0 |
| messaging | `lib/messaging`, `lib/pricing`, `common/chat`, `common/thread`, `domain/delivery`, `domain/attachments`, `feature/conversations` | 44.0 |
| calls | `lib/calls`, `lib/media`, `common/media`, `domain/call_media`, `feature/calls` | 42.1 |
| broadcast | `lib/broadcast`, `feature/broadcast` | 5.6 |
| node | `domain/node`, `app/node` (not `tools/`) | 2.3 |
| app (stays) | `app`, `gui`, `feature/product`, `feature/settings`, `lib/ui`, `lib/ai`, `feature/ai`, `common/ui` | 54.2 |

Every directory under `src/lib`, `src/domain` and `src/feature` must be in the map; the gate fails on one
that is not.

### Edges against the order

Include sites, tests included (a capability's tests move with it). The gate started from 11 edges, 67 sites;
moving misplaced tests (P1) took it to 10 edges, 53 sites (`scripts/check/capability_edges_allowlist.txt`).

| Edge | Sites | What it is |
|------|-------|------------|
| people → messaging | 21 | thread / chat types and the direct-message port used from identity code; `AccountCard` (identity) lives in `lib/messaging` |
| net → messaging | 10 | chat DTOs the backend client carries (relay envelope, history) |
| messaging → calls | 2 | `CallTypes` and the call media protocol id in the hub's ports |
| base → messaging | 2 | `foundation/runtime/BackgroundSyncScheduler` (stays app-side) and `common/thread`'s history types naming the relay envelope |
| people → net | 5 | directory and blob clients for profile icons |
| messaging → app | 5 | AI's tool registry and structured text, `lib/ui` types, from the conversations feature |
| messaging → node | 3 | the hub naming node services and mesh executors |
| node → messaging | 2 | `NodeMetrics` reading chat types |
| calls → broadcast | 2 | calls naming broadcast |
| node → calls | 1 | |

The heavy directions are allowed: messaging → people (159), messaging → net (35), calls → people (26),
calls → messaging (25), broadcast → calls (13).

### Capability → base

Every capability uses `foundation` and `common`. What they use of `foundation`: `crypto` (`CryptoUtil`,
ML-DSA, the PSK session store, the hybrid KEM, attachment ciphers), `runtime` (`AppRuntime`, `OwnerOutbox`,
`OwnerSteps`), `data` (`Config`, `MeshRole`, `UserPreferences`, `PricingTypes`), `platform`
(`NetworkMonitor`) and `i18n` (`Tr()` calls in call and messaging logic). P2 thins this; the runtime core
already left for pp-cpp-common (§ Runtime core).

## Target

```
pp-cpp-common (+ runtime) ──────┬─► pp-cpp-crypto ─► pp-cpp-amp ─► pp-cpp-mesh
                                │
           base ─► people ─┬─► net ───────────┐
                           ├─► social ────────┼─► messaging ─► calls ─► broadcast
                           │                  │
                           └──────────────────┴─► node (people, net)

apps:  pp-browser (UI, AI, composition)   pp-node (headless roles)   VPN client   ledger apps
```

| Capability | May include (besides itself) | Notes |
|------------|------------------------------|-------|
| people | base | identity: keys, cards, contacts, devices, the locker format (CR008) |
| net | base, people | the Brief backend client: registration, directory, blobs, push — deployment-specific (CR008) |
| social | base, people | posts + the content store; never above messaging (CR009) |
| messaging | base, people, net, social | records, delivery, attachments, the conversations hub, pricing (CR008) |
| calls | … + messaging | messaging carries call signaling |
| broadcast | … + calls | calls + broadcast are "realtime" (CR003); calls never names broadcast |
| node | base, people, net | node services and roles; reaches messaging and calls only through ports the app wires |
| app | anything | pp-browser's shell, GUI, AI (CR007), composition |

**When a second app needs capabilities** (CR005), they move to **one capabilities repo** (CR006) holding the
same target sets and the same gate. pp-node is the expected first: it would become its own app, picking
people, net and node from that repo.

## Rules

1. **The base stays thin.** Shared code holds mechanisms, not product: no config file formats, data paths,
   platform hooks, branding or user-facing text. Capabilities take what they need from the app as values
   and ports (paths, settings, network monitor), and report errors as codes. The app turns codes into text
   (`i18n` stays in apps).
2. **No edge against the order, checked.** `check_capability_edges.py` runs in CI's lint job: a new edge,
   or one that grows, fails; the allowlist only shrinks.
3. **Layers inside, capabilities across** (CR011). The layer rules of
   [SRC_LAYOUT](../../docs/architecture/SRC_LAYOUT.md) hold inside every capability; a capability's DTOs under
   `common/<capability>/` belong to it.
4. **The app owns the version set.** A capabilities repo pins dependencies only for its own CI; each app keeps
   one pin list that wins (as pp-cpp-mesh: "a consumer that already has the targets keeps its own").
5. **A capability owns its contracts and tests.** Wire, disk and HTTP contracts, unit and in-process tests move
   with it. Cross-capability tests (the app lab, the hard lab) stay with the apps that integrate them. Formats
   with more than one implementation keep specs and vectors in a contracts repo both sides pin (CR010).
6. **Move only on demand** (CR005), and only a boundary that has held.

## Runtime core → pp-cpp-common

Moved in pp-cpp-common **v1.3.0** (`common/runtime/`, namespace `pp`, with its tests): `AppRuntime`,
`CoordinatorThread`, `OwnerThread` / `OwnerTasks` / `OwnerOutbox` / `OwnerSteps`, `RuntimeOwnerExecutor`,
`ThreadRuntime`, `WorkerDispatch`, and `QueueOwnerExecutor` for tests. pp-browser bridges the names into `pbr`
(`common/PbrCompat.h`).

- **Owners are registered, not named by the runtime:** `AppRuntimeConfig::owners` lists id + thread name (ids
  below `kMaxOwnerThreads`); a drain passes through each registered owner. pp-browser's live in
  `foundation/runtime/ProductOwners.h` (`kMediaSessionsOwner`, `pp-media-sess`), with `ProductRuntimeConfig()`
  and its own `ManualOwnerRuntimeConfig()` for tests.
- **No product version in the shared runtime:** `pp_foundation_runtime_core` is now pp-browser's small target
  (the product's owners on top of `pp_common`) and still carries `PP_BROWSER_VERSION` /
  `PP_BROWSER_RELEASE_VERSION`.
- `AppLifecycle`, `BackgroundSyncScheduler`, `AppVersion`, `ProductBranding`, `FramePacing` stay in pp-browser.

## Before anything moves (CR012)

- **Pins:** one pin list per app and a local mode that builds sibling checkouts together
  (pp-ledger-apps' `PpFetchPin.cmake` as the start), so a capability + UI change stays one local build.
- **Vendored third-party code** (curl, BoringSSL, SQLite, …) stays committed, in one shared home rather than a
  copy per repo.
- **Visibility:** public under people-post, like the pp-cpp-* repos.

## Costs

- After a move, a change spanning a capability and the UI becomes a release plus a pin, as amp → mesh →
  pp-browser is today. Moving only on demand, and one repo rather than five, keep that rare.
- Mobile builds fetch one more dependency (already the pattern for amp / mesh / ui).
- The app lab drives capabilities through pp-browser's presenters (`pp-app-probe`), so it stays in
  pp-browser and tests a pinned set.

## Open questions

1. **Namespaces and include paths.** Today everything is `pbr::` (through `PbrCompat.h`) and
   `#include "lib/messaging/…"`. A moved capability needs its own (mesh uses `mesh::` and `mesh/…`). Proposal:
   settle the scheme during P3 while renames are in-repo, so a move is mechanical.
2. **The contracts repo** (CR010): its name, layout (spec + vectors per format) and how TS services pin it.
3. **The shared home for vendored deps** (CR012): pp-cpp-common, or a repo of its own.
