# Capability repos — design

**Goal:** pp-browser's capabilities — messaging, realtime (calls + broadcast), AI, node, identity — become
libraries in their own repos that pp-browser and other apps (a VPN client, the ledger apps) **pick**, like
they already pick [pp-cpp-amp](https://github.com/people-post/pp-cpp-amp) and
[pp-cpp-mesh](https://github.com/people-post/pp-cpp-mesh). pp-browser becomes the client and integrator:
UI, app composition, and the version set it ships.

**Non-goal:** one big "engine" repo. It would carry every shared dependency, and every capability change
would touch it.

## Where we are (measured 2026-10-29, `develop` at #404)

### Sizes (kLOC, non-test)

| Capability | Today's paths | kLOC |
|------------|---------------|------|
| calls (+ media) | `lib/calls`, `lib/media`, `common/media`, `domain/call_media`, `feature/calls` | 41.6 |
| messaging | `lib/messaging`, `common/chat`, `domain/delivery`, `domain/attachments`, `feature/conversations` | 36.0 |
| AI | `lib/ai`, `feature/ai` | 14.4 |
| people (identity, contacts) | `lib/people`, `domain/peers` | 5.6 |
| broadcast | `lib/broadcast`, `feature/broadcast` | 5.6 |
| net (org backend, registration) | `lib/net`, `domain/registration` | 3.8 |
| content | `lib/content`, `lib/posts` | 2.8 |
| node | `app/node` (not `tools/`), `domain/node` | 2.3 |
| pricing | `lib/pricing` | 1.9 |
| *foundation* | `foundation/*` | *22.4* |
| *app / gui* (stays) | `app`, `gui`, `feature/product`, `feature/settings`, `lib/ui` | *43.3* |

### Capability → capability include edges

No capability reaches into app/gui (one stray messaging include aside). Every cycle has a heavy direction
and a thin back edge; the thin edges are the work:

| Heavy direction (include sites) | Back edge to cut |
|---------------------------------|------------------|
| messaging → people (90) | people → messaging (12) |
| messaging → net (29) | net → messaging (5) |
| messaging → pricing (16) | pricing → messaging (1) |
| calls → messaging (10) | messaging → calls (2) |
| broadcast → calls (7) | calls → broadcast (2) |
| AI → messaging (8) | messaging → AI (2) |
| node → net (4), node → people (3) | node → messaging (1), node → calls (1), messaging → node (2) |
| people → net (3) | net → people (2) |

About 30 include sites in all. Re-measure before each phase (the script is in [PHASES § P0](PHASES.md#p0--measure-and-gate)).

### Capability → base

Every capability uses `foundation` (messaging 155 include sites, calls 66, AI 40, people 34, node 23 …)
and `common`. Inside `foundation`, what they use is: `crypto` (`CryptoUtil`, ML-DSA, the PSK session
store, the hybrid KEM, attachment ciphers), `runtime` (`AppRuntime`, `OwnerOutbox`, `OwnerSteps`), `data`
(`Config`, `MeshRole`, `UserPreferences`, `PricingTypes`), `platform` (`NetworkMonitor`) and `i18n` (32
`Tr()` calls in call and messaging logic).

## Target

```
pp-cpp-common (+ runtime core) ─┬─► pp-cpp-crypto ─► pp-cpp-amp ─► pp-cpp-mesh
                                │
                                └─► identity (people + net + pricing)
                                        ├─► messaging ─► realtime (calls + broadcast)
                                        ├─► AI (messaging through ports it declares)
                                        └─► node (+ mesh, ledger)

apps:  pp-browser (UI + composition)   pp-node (headless roles)   VPN client   ledger apps
```

| Repo | Holds | Depends on |
|------|-------|------------|
| pp-cpp-common | + **runtime core** (owner threads, coordinator, worker dispatch) | — |
| pp-cpp-crypto | + the crypto `foundation/crypto` adds today (message/attachment ciphers, PSK store) | common |
| identity | people (identity store, contacts, device linking), net (org backend, registration), pricing | common, crypto, mesh types |
| messaging | records, delivery, attachments, conversations hub | identity, mesh |
| realtime | calls, call media, broadcast (they share `lib/media`; broadcast alone is small) | messaging (signaling carrier), mesh |
| AI | planner, tools, prompts; messaging and other capabilities through ports | identity |
| node | node services, node roles (relays, DHT, directory, ledger, a VPN exit later) | identity, mesh, ledger |

## Rules

1. **The base stays thin.** Shared repos hold mechanisms, not product: no config file formats, data paths,
   platform hooks, branding or user-facing text. Capabilities take what they need from the app as values
   and ports (paths, settings, network monitor), and report errors as codes. The app turns codes into
   text (`i18n` stays in apps).
2. **No cycles, checked.** A capability may include only capabilities below it. A check script (like
   pp-node's link allowlist in `cmake/PpBrowserNoUiDeps.cmake`) fails the build on a new upward edge,
   in-repo first and then per repo.
3. **The app owns the version set.** Capability repos pin dependencies only for their own CI; the app's
   pins win (pp-cpp-mesh already works this way: "a consumer that already has the targets keeps its
   own"). Each app keeps one version list, so two capabilities never bring two versions of a third.
4. **A capability owns its contracts and tests.** Wire, disk and HTTP contracts, unit and in-process
   tests move with it. Cross-capability tests (the app lab, the hard lab) stay with the apps that
   integrate them and test the pinned set.
5. **Split in-repo first.** Each capability becomes its own CMake target set with a clean boundary inside
   pp-browser before it moves; a repo move is then close to mechanical, and only happens once the boundary
   has held for a while.

## Runtime core → pp-cpp-common

`foundation/runtime` is 3.1k lines (+1k tests). Its core already builds only on pp-cpp-common (`WorkerPool`,
`Executor`, `SerialExecutor`, `SequencedTaskRunner`, `Lifetime`, `Logger`, metrics), plus pp-browser's
`PbrCompat.h` namespace bridge (moved code uses `pp::` directly).

| Part | kLOC | Goes to |
|------|------|---------|
| `AppRuntime`, `CoordinatorThread`, `OwnerThread` / `OwnerTasks` / `OwnerOutbox` / `OwnerSteps`, `RuntimeOwnerExecutor`, `ThreadRuntime`, `WorkerDispatch` | ~2.2 | pp-cpp-common |
| `AppLifecycle`, `BackgroundSyncScheduler`, `AppVersion`, `ProductBranding`, `FramePacing` | ~0.6 | stay in apps (lifecycle, versions, branding, frame pacing) |

Changes it needs:

- `OwnerThreadId` (today a pp-browser enum, `MediaSessions`) becomes owners an app or capability
  registers at startup; a shared runtime cannot name product threads.
- `pp_foundation_runtime_core` stops carrying `PP_BROWSER_VERSION` / `PP_BROWSER_RELEASE_VERSION`.
- `BackgroundSyncScheduler`'s include of `common/chat/MessagingLimits.h` stays with it (app side).

## Costs

- A change spanning a capability and the UI becomes a capability release plus a pin, as amp → mesh →
  pp-browser is today. In-repo boundaries first (rule 5) keep that to boundaries that have proven stable.
- Mobile builds fetch more dependencies (already the pattern for amp / mesh / ui).
- The app lab drives capabilities through pp-browser's presenters (`pp-app-probe`), so it stays in
  pp-browser and tests a pinned set.
