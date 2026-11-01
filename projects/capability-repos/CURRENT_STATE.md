# Capability repos — current state

**Updated:** 2026-11-01  
**Phase:** P0 and P1 done (no edge against the order); P2 gated (CR015), runtime core done

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
- **P2, runtime core:** in pp-cpp-common v1.3.0 (`common/runtime/`), owners registered by the app
  (`foundation/runtime/ProductOwners.h`).

## Next

1. P2: the product edges down (`Config`, `MeshRole`, `Tr()`, platform hooks); the hub's shell (CR018);
   crypto additions into pp-cpp-crypto.
2. Settle CR019 (presenters) before anything moves.
3. Contracts repo (CR010) when convenient.
