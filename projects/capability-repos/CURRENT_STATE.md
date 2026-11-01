# Capability repos — current state

**Updated:** 2026-10-31  
**Phase:** P0 done; P1 under way; P2's runtime core done

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
- **P2, runtime core:** in pp-cpp-common v1.3.0 (`common/runtime/`), owners registered by the app
  (`foundation/runtime/ProductOwners.h`).

## Next

1. P1: one edge per PR.
2. P2: `foundation/data` / `platform` / `Tr()` out of capability code; crypto additions into pp-cpp-crypto.
3. Contracts repo (CR010) when convenient.
