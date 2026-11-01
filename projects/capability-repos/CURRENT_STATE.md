# Capability repos — current state

**Updated:** 2026-11-01  
**Phase:** P0, P1 and P2 done; P3: capability folders (CR022), people, net, social, delivery, pricing and messaging moved

## Done

- Measured sizes, capability edges and base usage ([DESIGN § Where we are](DESIGN.md#where-we-are-measured-2026-10-30-develop-at-0d77a6a35)).
- Direction revised (CR005–CR012): move only for a second app, into one capabilities repo; AI stays in the
  app; identity is people, net its own, pricing inside messaging; posts + content are "social"; contracts and
  vectors in their own repo.
- **P0:** `scripts/check/check_capability_edges.py` in CI's lint job, starting from 11 edges / 67 include
  sites against the order (`capability_edges_allowlist.txt`).
- **P1, misplaced tests:** calls logic tests to `lib/calls/tests`, `common/chat` and `common/ui` tests beside
  their code — 10 edges / 53 sites left.
- **CR013:** lib peers include and link each other down the capability order (the layer checks read the
  pairs from the capability gate).
- **P1, people → messaging:** 21 sites → 0 (cards, key stores, `DevicePeerIdFromKey` into `lib/people`; the
  people + thread compositions into `lib/messaging`, `feature/conversations`, `feature/ai`) — 9 edges / 32
  sites left.
- **P1, net → messaging:** 10 sites → 0 — the relay's message client (`HttpRelayClient`), the relay and
  peer ports (`RelayClients.h`), the message signing builders and the relay mocks move into messaging; net
  keeps the relay API header, push and its own clients — 8 edges / 22 sites left.
- **P1, four small edges:** node → messaging, node → calls, messaging → calls, calls → broadcast → 0 — the
  product's L4 protocol ids and `ProductMeshProtocols()` in `common/ProtocolIds.h` (base), the people-picker
  ports (`MessagingPeoplePickerPorts`) in `feature/product`, an unused broadcast include dropped from calls —
  4 edges / 15 sites left (people → net, messaging → app, messaging → node, base → messaging).
- **P1, base → messaging and people → net:** 7 sites → 0 — the relay poll's cadences in
  `BackgroundSyncScheduler.h`, `ContextBudget` at the common root; `DirectoryShadowCache` on mesh's
  `IDirectoryClient`, the profile-icon fetch and upload in `lib/net` — 2 edges / 8 sites left
  (messaging → app, messaging → node).
- **P1, messaging → app:** 5 sites → 0 — `MessagingTools` (messaging's AI tools) and its tests in
  `feature/product`, where messaging meets AI; the chats list's preview prose a `ChatPresenter` port
  (`Callbacks::prose`) the GUI and pp-app-probe fill with `StructuredTextParser::PlainText`;
  `kGroupMemberLimit` in `MessagingLimits.h` — 1 edge / 3 sites left (messaging → node).
- **P1, messaging → node:** 3 sites → 0 — the conversations hub no longer builds the mesh: `ProductMesh`
  (`feature/product`) owns the `MeshHost`, gives it the profile's role from `domain/node` (hosting and
  serving, DHT and directory, its own directory row) and lends it to the hub (`HubMeshOwner`: start,
  configure queries, apply serving, stop, release). `MeshExecutors` moved to pp-cpp-mesh's `mesh/types/`
  (v3.0.0), so `AppRuntimeMeshExecutors` lives in `foundation/runtime`. The allowlist is empty.
- **P2 gated (CR014 – CR020 drafted):** the product's base (`foundation/data`, `platform`, `i18n`) is the gate group
  **product**, apps only; pp-node is the app **node_app**; the gate counts CMake links. Pricing DTOs moved to
  `lib/pricing` and five stale links dropped — 68 include / 14 link sites left.
- **P2, people and node off the product's base:** the config slices they take as values (`MeshConfig`,
  `ServiceEndpoints`) and the mechanisms parked there (`SchemaVersion`, `DialWorthyMultiaddrs`) into `common/`;
  the profile manifest stays the product's (`ProfileManifest`); the secure-blob port into `common/` with the OS
  store in `foundation/platform`, which the app hands to the retired-Keychain cleanup it now runs; pp-node
  passes the org seed's role in; net's client factory takes endpoints — 60 include / 10 link sites left.
- **CR020, delivery:** accepted and mapped — `lib/delivery` (sessions, envelopes, codecs, the relay client),
  `common/chat` + `common/thread` (the records and store ports) and `domain/delivery` sit below messaging and
  calls, with pricing as its own capability; calls and broadcast use no messaging — 60 include / 9 link sites
  against the product's base left, no other edge.
- **TLS, CR019, CR018 step 1:** the per-OS TLS trust is `foundation/tls`; the presenters are
  `feature/presenters` (the app's); the OS network monitor is the mesh owner's, its value types the base's —
  51 include / 8 link sites against the product's base left.
- **CR018 steps 2–4:** LAN mDNS and the mesh directory cache are the mesh owner's; the backend's clients are
  `lib/delivery/BackendClientSet`, the platform defaults handed in by `ProductStack` — 49 include / 8 link sites
  left. The seed-reservation renewal waits for `MeshConnectivity` to move.
- **P2, the hub and calls take values:** `HubSettings` (projected in `feature/product`), the device's role as a
  value, the effective mesh config published to calls — 24 include / 6 link sites left (messaging 11, calls 9,
  base 3, broadcast 1): `Tr()` (9) and platform mechanisms (push registrar, call system UI, user hints, media and
  file helpers, assets), plus config secrets in the base.
- **P2, platform mechanisms:** into the base, behind ports, or to the app (see CR015's notes) — 14 include / 4 link
  sites left: `Tr()` (messaging 4, calls 4, base 2), the platform's user-hint keys (calls 2), config secrets (base 1).
- **P2, text and the rest:** `Tr()` replaced by the base's `Localized(key)` port (the app's catalogs answer), the
  platform tip key aliased by the app, the config-secrets store in `foundation/data` — 0 include / 0 link sites: the
  allowlist is empty.
- **Codes and CR018 step 5:** call history rows are codes the GUI words; the neutral mesh objects and the seed
  reservation are `ProductMesh`'s — the hub owns no mesh object.
- **P2, crypto (CR021):** pp-cpp-crypto v1.1.0 (base64url, BLAKE2b-256, `ReplayWindow`) pinned; delivery's crypto
  formats in `lib/delivery`, `MediaKeyWrap` in `lib/calls`. P2 is done.
- **P2, runtime core:** in pp-cpp-common v1.3.0 (`common/runtime/`), owners registered by the app
  (`foundation/runtime/ProductOwners.h`).

- **P3, capability folders (CR022):** people in `capabilities/people/src/` (`lib/people`, `domain/peers`,
  `common/directory` → `pp_people_common`); the gate and include checks read the folders
  (`scripts/check/source_trees.sh`) and the gate fails on a foreign path; its 137 tests are labelled
  `capability:people`, and a PR that touches neither the folder nor the base skips them.
- **P3, net's folder:** `lib/net` and `domain/registration` in `capabilities/net/src/`; a folder's
  `CMakeLists.txt` calls `pp_browser_capability(<name>)`, which makes its `src/` an include root of the targets the
  helpers make there. CI's filters chain (net's is people's plus its folder); 81 tests labelled `capability:net`.
- **P3, social's folder:** `lib/posts` and `lib/content` in `capabilities/social/src/` (29 tests).
- **P3, delivery's folder:** `lib/delivery`, `domain/delivery` and its records (`common/chat`, `common/thread` →
  `pp_delivery_common`, which ai, ui, calls, pricing and broadcast link for the record types) in
  `capabilities/delivery/src/` (63 tests).
- **P3, pricing's folder:** `lib/pricing` in `capabilities/pricing/src/`.
- **P3, messaging's folder:** `lib/messaging`, `domain/attachments`, `feature/conversations` in
  `capabilities/messaging/src/` (484 tests). A feature module in a capability folder links only what it names (no
  `pp_base`, no `pp_feature` in its tests); that dropped the hub's one hidden UI edge (`SDL_GetTicks` for the
  reachability banner's timer → `steady_clock`).

## Next

1. The other capability folders, in order: calls, broadcast, node.
2. Contracts repo (CR010) when convenient.
3. P3's namespaces when a second app needs a capability.
