# Capability repos — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

## P0 — Measure and gate

- [x] `scripts/check/check_capability_edges.py`: groups `src/` by capability ([DESIGN § Capabilities in the
      tree](DESIGN.md#capabilities-in-the-tree)), counts `#include` edges between groups (tests included), and
      fails on any edge against the order unless `capability_edges_allowlist.txt` holds it with exactly that
      count; fails on a directory under `lib/`, `domain/` or `feature/` no capability owns.
- [x] In CI's lint job beside the other `scripts/check/` guards.

## P1 — Cut the back edges (in-repo)

One PR per edge, lowering its allowlist line ([DESIGN § Edges against the order](DESIGN.md#edges-against-the-order)):

- [x] Misplaced tests first: calls logic tests under `feature/conversations/tests` → `lib/calls/tests`;
      `common/chat` and `common/ui` tests under `common/tests` → `common/chat/tests`, `common/ui/tests` (14 sites).
- [x] people → messaging (21): lib peers follow the capability order (CR013); cards and key stores down
      into `lib/people`, people + thread compositions up into messaging
- [x] net → messaging (10): the relay's message client and the relay / peer ports are messaging's
      (`lib/messaging/RelayClients.h`, `HttpRelayClient`, `RelayMessageSignPayload`, mocks in
      `lib/messaging/testing`); net keeps the relay API header and its own clients
- [x] base → messaging (2): the relay poll's cadences into `BackgroundSyncScheduler.h`; `ContextBudget` (a
      `Config` section) to the common root
- [x] people → net (5): `DirectoryShadowCache` on mesh's `IDirectoryClient`; profile icons (`ProfileIconFetch`,
      `ProfileIconClient`) into `lib/net`; the peers tests on fake directories
- [x] messaging → app (5): messaging's AI tools (`MessagingTools`) and their tests into `feature/product`;
      the chats list's preview prose a `ChatPresenter` port the GUI fills with AI's parser (CR007);
      `kGroupMemberLimit` into `MessagingLimits.h`; an unused `lib/ui` include dropped
- [x] node → messaging, node → calls, messaging → calls, calls → broadcast: the product's L4 protocol ids and
      `ProductMeshProtocols()` into the base (`common/ProtocolIds.h`); the people-picker ports into
      `feature/product`; an unused broadcast include out of calls
- [x] messaging → node (3): the mesh leaves the conversations hub — `ProductMesh` (`feature/product`) owns
      the `MeshHost` and gives it the role (`domain/node`); the hub borrows it through `HubMeshOwner`.
      `MeshExecutors` into pp-cpp-mesh's `mesh/types/` (v3.0.0), so `AppRuntimeMeshExecutors` is
      `foundation/runtime`'s

## P2 — Thin the base (in-repo, then pp-cpp-common / pp-cpp-crypto)

- [x] Gated (CR015 – CR017): `foundation/data`, `foundation/platform`, `foundation/i18n` are the group
      **product**, only the apps (`app`, `node_app`) may use it; the gate counts CMake links too. Baseline
      68 include / 14 link sites (`capability_edges_allowlist.txt`). Pricing DTOs into `lib/pricing` (17 sites);
      stale links dropped (conversations → calls / media, calls → broadcast, delivery tests → node,
      content → data).
- [x] Capabilities stop reading `foundation/data` (`Config`, `UserPreferences`, `MeshRole`) and
      `foundation/platform`: the app passes values and ports in (`HubSettings`, `HubMeshOwner`,
      `AttachmentPlatform`, the push registrar port, `MeshRoleOf`). People and node done (the config slices
      `MeshConfig` and `ServiceEndpoints`, `SchemaVersion`, `DialWorthyMultiaddrs` and the secure-blob port into
      `common/`; the OS store in `foundation/platform`, handed in by the app); net has `CurlSsl` left.
- [x] The hub's shell thins (CR018): mesh glue, the neutral mesh objects and the seed reservation into
      `ProductMesh`; backend clients a delivery-level set handed in.
- [x] Delivery below messaging and realtime (CR020): the gate maps delivery and pricing; calls and broadcast
      no longer include or link messaging.
- [x] `Tr()` out of capability code: text by key through `common/Localized.h`, the app's catalogs answer.
- [x] Codes where they pay: call history rows (`CallSummaryRow::Outcome`). Stored previews and call statuses stay on
      keys through the port (CR015's note).
- [x] Runtime core into pp-cpp-common v1.3.0 ([DESIGN § Runtime](DESIGN.md#runtime-core--pp-cpp-common)):
      registered owners instead of `OwnerThreadId::MediaSessions`, no product version defines.
- [x] The crypto pp-browser adds: generic pieces into pp-cpp-crypto v1.1.0 (base64url, BLAKE2b-256, `ReplayWindow`;
      `CryptoUtil` now names pp-cpp-crypto's); pp-browser's message formats into delivery (`lib/delivery`:
      session keys, AAD, PSK bundles / fingerprints, `key_init`, the PSK store port, the attachment cipher;
      `MediaKeyWrap` into `lib/calls`). pp-cpp-crypto keeps its rule: no product wire formats.

## P3 — Capability targets (in-repo)

- [x] Each capability builds as its own CMake target set and links only what is below it — its folder's targets
      (CR022), whose link closures hold nothing above them.
- [ ] The namespace and include-path scheme for capabilities ([DESIGN § Open questions](DESIGN.md#open-questions)),
      renamed in-repo.
- [x] Capability folders (CR022), one change each, in the order: people, net, social, delivery,
      pricing, messaging, calls, broadcast, node. Each: `git mv` into `capabilities/<name>/src/`,
      its `CMakeLists.txt` with the CTest label, a changes-filter output and its `-LE` skip in `build.yml`.

- [ ] Calls repo-ready (CR023): [x] owns its tests, [ ] `media` split, [ ] base folder, [ ] standalone build + CI,
      [ ] host contract + README / docs.
- [x] Ideas in capabilities, the database in the app (CR024): [x] delivery's ports and in-memory stores,
      [x] messaging's store ports and in-memory stores (contract-tested against SQLite),
      [x] `src/lib/storage` with the SQLite stores and the profile database, [x] calls' stores,
      [x] people's and social's stores (peer cards, posts, content's object index).

## Contracts repo (any time, CR010)

- [ ] Specs and golden vectors for formats with more than one implementation (posts, the account locker, the
      KEM binding) in their own repo; pp-browser and the TS services (app-posts, app-static-api) pin it and
      test against it instead of copied vectors.

## P4 — The capabilities repo (when a second app needs it, CR005 / CR006)

Not scheduled. Before it: one pin list per app and a sibling-build mode; one home for vendored deps (CR012).

- [ ] Create the capabilities repo (people-post, public) with the target sets that app needs, the gate, and
      their contracts and tests; release; pin in every app; pp-browser drops its copy.
- [ ] Expected first: pp-node as its own app (people, net, node; with `packaging/pp-node`, the N-* smokes,
      the hard lab and `pp_node_closure.sh`).
