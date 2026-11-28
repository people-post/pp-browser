# Capability repos — current state

**Updated:** 2026-11-27  
**Phase:** P0, P1 and P2 done; P3: every capability in its folder (CR022); calls repo-ready (CR023) done; storage in the app (CR024) done; the docs split (CR025) step 1 done; CR012's prerequisites for P4 done in pp-browser (pins, sibling mode, vendored-deps manifest); P4's shape is layered repos (CR027), pp-node repo-ready in place next (CR028)

## Done

- Measured sizes, capability edges and base usage ([DESIGN § Where we are](DESIGN.md#where-we-are-measured-2026-10-30-develop-at-0d77a6a35)).
- Direction revised (CR005–CR012): move only for a second app, into one capabilities repo (since CR027, 2026-11-27:
  layered repos, each born on demand; pp-cpp-core and pp-node first); AI stays in the
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
- **P3, calls' folder:** `lib/calls`, `lib/media`, `common/media` (→ `pp_calls_common`), `domain/call_media` (still
  in headless configures, for pp-call-probe) and `feature/calls` in `capabilities/calls/src/` (594 tests); no hidden
  link to remove. `lib/media` keeps SDL3 and Opus as its own third-party links.
- **P3, broadcast's folder:** `lib/broadcast` and `feature/broadcast` in `capabilities/broadcast/src/` (94 tests). The
  call probe (an app tool) had reached the product's base through the feature bundles; it names
  `pp_foundation_platform` and `pp_foundation_data` itself now.
- **P3, node's folder:** `domain/node` in `capabilities/node/src/` (6 tests); `src/domain/` keeps only the app's
  aggregate. Every capability is in its folder: 1521 of 2910 tests carry a capability label; the rest are the base's
  and the app's. Every capability target's link closure holds only what is below it (checked with CMake's link graph).

- **CR023 step 1 and CR024 stage 1:** calls owns its tests (17 composed call tests out of `feature/product`, 808
  labelled); the one checking the app's wording of call lines stays with the app. Delivery's session idea is
  `PeerSessionKey` / `PeerSessionRecord` / `IPeerSessionStore` (a thin port; an epoch's key, verification and key
  bundles are delivery's on top), the control lane's seen ids a port, and `InMemoryDeliveryStore` keeps threads and
  peer sessions on shared rows for tests — found on the way: delivery's SQLite PSK store wrote into messaging's
  `chat_targets` table and shared its epoch with the thread store.
- **CR024 stage 2a:** messaging's stores are ports (`IGroupRosterStore`, `IThreadLocalPrefsStore`,
  `IThreadMentionsStore`; the SQLite ones `Sqlite*`) with in-memory ones in `lib/messaging/testing`; a contract test
  runs every case on both.
- **CR024 stage 2b:** `src/lib/storage` (the app) holds the profile database and every SQLite store of delivery and
  messaging; `ProductStack` hands `OpenSqliteProfileStores` to the hub (`ConversationsHub::SetStorage`, the ports in
  `lib/messaging/ProfileStores.h`). Delivery and
  messaging hold no SQL; their tests run in memory — the in-memory thread store learned what messaging relies on
  (the catalog row an append moves, the outbox, seq ranges) and "delete for me" became the port's own. The SQLite
  behaviours (schema, restart, transactions, many connections) are storage's tests. The tables keep their names
  (`chat_targets` included).
- **CR024 stage 3:** calls' stores are ports (`ICallSessionStore`, `ICallMediaKeyStore`; a fresh key and its wrap
  are calls', on the port) handed to `CallStack::InitializeStores` as `CallStores`, which `CallsParticipant` opens
  through storage (`OpenSqliteCallStores`); in-memory ones in `lib/calls/testing`, a contract test on both. Calls
  holds no SQL and no at-rest encryption.
- **CR024 stage 4, people and posts:** `PeerCardStore` is people's `PeerCards` (the checks, the merge, the floors)
  over an `IPeerCardStore` port, from `ProfileStores`; `PostStore` is social's `IPostStore` (`AddEntry` on the port).
  Their SQLite ones are storage's, in-memory ones in each capability's `testing/`, contract tests on both. People and
  posts hold no SQL; `pp_lib_people_cards` is gone (the cards are plain people code, headless too).
- **CR024 stage 4, content (CR024 done):** `ObjectIndex` is content's `IObjectIndex` port; `SqliteObjectIndex` is
  storage's (`object_index.db`, unchanged), `InMemoryObjectIndex` in `lib/content/testing`, a contract test on both.
  `CasStore` is one per profile, built by the hub on the index the app opens (`SetContentStorage`) and shared with
  the attachment workflow, the blob transport and Me → Storage; content's functions take it instead of a path. No
  capability holds SQL now.

- **CR023 step 2, the media split:** `capabilities/media` (devices, hardware codecs, `MediaEngine` — renamed from
  `CallMediaEngine`; `common/media_engine`) on the base only; calls' adaptation policy to `lib/calls` (the engine
  applies `MediaTargets`), the call's sounds to `lib/call_audio`, voice messages to messaging's `lib/voice`, QR
  scanning to the app's `lib/scan`. The ringback's audio-session hold is a `CallAudioSession` tone hold. Gate map,
  CI filters and labels updated.

- **CR023 step 3, the base's folder:** `capabilities/base/src` (common, foundation's runtime / TLS / error /
  diagnostics / crypto / identity); `src/foundation` keeps the product's base (+ `product`: name, version). No
  capability target has `src/` as an include root any more (all 563 capability sources compile without it). Calls
  names its owner thread and the app registers it; the crash dump, the text layer and net take the product's identity
  as values.

- **CR025, the docs split, step 1 (2026-11-23):** where docs live is decided and mapped
  ([docs/README.md § Where docs live](../../docs/README.md#where-docs-live)); five finished projects in
  `projects/_archive/`; media-hop-reachability in pp-cpp-mesh (stubs here); a `docs/README.md` in every capability
  folder.

- **CR012's prerequisites (2026-11-23):** one pin list, [`cmake/PpPins.cmake`](../../cmake/PpPins.cmake) (pp-cpp-common,
  -crypto, -amp, -mesh, -ui, pp-ledger; pp-browser's and pp-node's), with the mechanism in `cmake/PpFetchPin.cmake` for
  another repo to copy; sibling mode, `-DPP_SIBLINGS_DIR=..`, every library from a checkout beside the repo, warned
  when a checkout's HEAD is not its pin; `third_party/UPSTREAM.json` the manifest of every vendored copy (and
  `third_party/README.md` what comes from the libraries' trees). The same compile commands and targets as before; full
  ctest, and a sibling build against checkouts at the pins, pass. The shared home across repos waits for a second repo
  (CR012's notes).

- **CR023 steps 4–5, calls repo-ready (2026-11-23):** calls builds and tests on its own —
  `cmake -S capabilities -DPP_CAPABILITIES=calls` (calls, base, people, net, delivery, pricing, media; no app), and
  CI's `capabilities` job builds calls' tests so and runs its 784 on every PR that touches calls or what is below it. A
  link above calls fails that configure; the gate holds the standalone table to the order. The host contract is
  [capabilities/calls/README.md](../../capabilities/calls/README.md); the Android hooks media called on the app's
  activity by name are a port (`MediaHost`) the app fills (`src/app/MediaHost_Android.cpp`).

## Defaults taken (owners may revisit)

- **The shared product base goes into pp-cpp-core** (CR028, 2026-11-27), not left with pp-browser: both apps need it
  identical (one data layout, one profile model, the same backends), and the pp-node repo could not otherwise build without
  pp-browser. The desktop's `AppConfig`, i18n and GUI platform stay with pp-browser (Hongwei).

- **pp-cpp-ui's implicit `../pp-cpp-ui` is gone** (CR012): a developer who built a checkout beside the repo without
  asking now builds the pinned tag, and configure says so once. Pass `-DPP_SIBLINGS_DIR=..` or `-DPP_CPP_UI_SOURCE_DIR=…`
  (Kenneth: the UI engine's main user of it).
- **`cmake/PpPins.cmake` is owned by @yhw1630** in CODEOWNERS (it holds Amp's and the mesh's tags, which only he owned);
  pp-cpp-common, -crypto and -ui bumps were also Kenneth's to approve.

## Next

1. Contracts repo (CR010): pp-specs, now that release 1 has shipped — CR025 step 2. **Started 2026-11-27:**
   [pp-specs](https://github.com/people-post/pp-specs) created (public), the 17 vector files imported byte for byte with a
   sha256 manifest, tagged `v1.0.0`; pp-browser's tests read them at that pin and its copies are gone. Left: the relay
   and the three libraries pin it too; the shared contracts and the cross-repo programs (this project included) move. First, because [node-relay](../node-relay/README.md)'s
   relay protocol and its vectors are specified there (its NR1).
2. P3's namespaces when a second app needs a capability.
3. The same standalone build for the next capability a second app asks for (net and base for the operator app,
   below): `-DPP_CAPABILITIES=net` configures today; a CI job of its own when it leaves.
4. **pp-node repo-ready in place (CR028), now:** its own config type over the same keys; `capabilities/product`, the
   headless product base both apps use; a standalone pp-node build with its CI job. It moves nothing.
   - **Stage 1, in review (#809):** pp-node works on `NodeConfig` (`mesh`, `registration`, `data_dir`,
     `relay.content_sha256`, `node`); `apps/node/src/app/node/NodeConfigLoad` is the one place it meets `AppConfig`, loading with
     the app's loader. `node_config_test` pins what the node reads, for stage 2's loader of its own. pp-node now warns
     when a built-in backend ignores the file's `registration`.
   - **Stage 2, in review (one PR, a commit per step):** 2a the config file's shared sections out of the desktop's
     `ConfigJson` (`ConfigSectionsJson`), `MeshRole.h` off `Config.h`; 2b pp-node's own loader (`LoadNodeConfig`), held
     to the app's by `node_config_test` (21 files × 4 builds); 2c `capabilities/product` (`pp_foundation_product`,
     `pp_product_platform`, `pp_product_data`), which pp-node links instead of the desktop's `pp_foundation_data` and
     `pp_foundation_platform_core`; 2d pp-node in a folder of its own, `apps/node/` (its include root never `src/`),
     which also configures on its own (`cmake -S apps/node`, the capabilities from `capabilities/` through
     `cmake/PpCapabilitiesRoot.cmake`), with a step in CI's `node-editions` job. The app's probes moved to `src/app/probes`.
5. **P4 (CR027), agreed 2026-11-27 (Hongwei):**
   - The ledger role leaves `node` for its own capability, `ledger` (in review), so pp-cpp-core needs no pp-ledger.
   - **pp-cpp-core**, created after that: base, product, people, net, node, with their history (`git filter-repo`);
     the vendored curl, zlib and asio move there (CR012's shared home); reviewers as pp-browser's (Hongwei, Kenneth,
     Leo); tag v0.1.0; pp-browser pins it and drops its copies; pp-ledger-apps pins it for the operator app.
   - Before that note: P4 (CR027) after node-relay's mailbox works: **pp-cpp-core** first (base, the shared product base, people, net,
   node; the operator app's relay certificate code is its first new use), then the **pp-node** repo (the app, both editions,
   the add-ons, its packaging and release) rather than moving code still being built.

## A second app is waiting (2026-11-22)

**pp-ledger-apps' operator app** needs the relay id, operator set and relay certificate code: net's
`capabilities/net/src/lib/net/RelayCertificate.*` and base's `capabilities/base/src/foundation/crypto/RelayId.*`,
with pp-specs `vectors/relay_certificate_vectors.json`
([pp-ledger-apps OP008](https://github.com/people-post/pp-ledger-apps/blob/main/projects/operator-app/DECISIONS.md)).
It is P4's first move (CR005), after release 1: not earlier, because pp-browser keeps its pins during the freeze.
Not pp-cpp-crypto: its README keeps product wire formats in the apps, and only the hashing, signatures, typed blobs
and PeerId in this code are crypto.

- **Before it:** CR012's prerequisites (one pin list per app and a sibling-build mode; one home for vendored deps) —
  done in pp-browser; the operator app keeps its own list with a copy of `cmake/PpFetchPin.cmake`. The vectors go to the contracts repo (pp-specs, CR010 / CR025 step 2) in the same step if it exists by then, since the relay reads them too.
- **A reference port:** pp-cpp-crypto branch `feat/relay-certificate` (commit 402421f; not merged, not to be released
  from there) carries the code with these vectors byte for byte, every case with the same verdict. Moving it here:
  - origin policy becomes the caller's: the shared code keeps the structural rules and takes an origin predicate, and
    pp-browser passes its card rule (`IsCertificateOrigin`: 128 bytes among other bounds), or it accepts certificates
    it refuses today;
  - errors become the shared code's own category (the verdict as the code), mapped here to `AppError::NewerFormat` and
    `Err::Network::RelayIdentityUnavailable` as `RelayCertificateCheck::ToError` does today;
  - it needs pp-cpp-common 1.5.0 or later (`ExtArea`).
