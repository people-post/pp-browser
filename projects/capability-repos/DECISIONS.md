# Capability repos — decisions

Living ADRs. Promote durable layout rules into [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md) when they
ship; mark superseded here.

---

## CR001 — Capability repos apps pick, not one engine repo

**Date:** 2026-10-29  
**Status:** accepted; amended by CR005 (move only for a second app) and CR006 (one capabilities repo, a target
per capability, not a repo each); its list amended by CR007 (AI stays in the app) and CR008 / CR009

**Context:** Messaging, calls and broadcast are about 83k of pp-browser's 179k source lines. One "engine" repo
for everything below the GUI would also carry most of `foundation`, `common`, `lib/people` and `lib/net`
(~900 include sites from the moved code), and every capability change would touch it.

**Decision:** Separate capability repos (identity, messaging, realtime, AI, node) on a thin shared base;
pp-browser and other apps pick the capabilities they need.

**Consequences:** A capability + UI change becomes a release plus a pin. Accepted, and contained by splitting
in-repo first (CR004).

## CR002 — Runtime core goes to pp-cpp-common

**Date:** 2026-10-29  
**Status:** accepted; done in pp-cpp-common v1.3.0 (2026-10-30) — DESIGN § Runtime core

**Context:** Every capability depends on the threading model (`AppRuntime`, owner threads, `OwnerOutbox`).
The core (~2.2k lines) already builds only on pp-cpp-common's executors, logger and metrics.

**Decision:** Move the runtime core into pp-cpp-common; app lifecycle, versions, branding and frame pacing
stay in apps. Owner threads become registered by apps and capabilities instead of a pp-browser enum.

## CR003 — Calls and broadcast are one repo ("realtime")

**Date:** 2026-10-29  
**Status:** accepted; amended by CR006 — "realtime" is the calls and broadcast targets of the one capabilities
repo, still with no cycle between them; amended by CR020 (realtime rides delivery, not messaging)

**Context:** They share `lib/media`, have a cycle between them (broadcast → calls 7 include sites, calls → broadcast 2),
and broadcast alone is 5.6k lines.

**Decision:** One realtime repo. Revisit if broadcast grows a life of its own.

## CR004 — In-repo boundaries before repo moves; the app owns the version set

**Date:** 2026-10-29  
**Status:** accepted; amended by CR005 (a boundary that has held moves only when a second app needs it)

**Context:** A repo split is cheap to do and costly to live with when the boundary still moves. With many
repos, two capabilities can pin two versions of a third.

**Decision:** Each capability first becomes its own target set inside pp-browser behind a gated boundary
(no new upward edges); it moves only once that has held. Capability repos pin dependencies for their own
CI only; each app keeps one version list that wins.

## CR005 — Move a capability only when a second app needs it

**Date:** 2026-10-30  
**Status:** accepted

**Context:** On 2026-10-30 no other app uses any pp-browser capability: the ledger apps pick pp-cpp-common,
-crypto and -ui, pp-ledger picks pp-cpp-amp, and the VPN client does not exist yet. A repo move makes every
later change that crosses it a release plus a pin.

**Decision:** The in-repo work (P0–P3: the gate, the cuts, a thin base, a target set per capability) goes
ahead now. A capability leaves pp-browser only when a second app needs it. pp-node is the first such app: it
already ships on its own.

**Consequences:** P4 is demand-driven, not a schedule. Until then the targets and the gate give the same
boundaries at no release cost.

## CR006 — One capabilities repo, a target per capability

**Date:** 2026-10-30  
**Status:** accepted; supersedes the repo-per-capability shape of CR001

**Context:** CR001 rejected one engine *target* that every app links whole. Separate repos per capability
fix that but cost one pin each, and two capabilities can pin two versions of a third.

**Decision:** Capabilities that leave pp-browser go to one repo, with a CMake target set per capability (and
options so a consumer builds only what it picks). An app fetches it once and links the targets it needs.

**Consequences:** One pin per app instead of five, and no version skew between capabilities. A consumer
fetches source it does not build. The capability order (DESIGN § Target) is enforced inside that repo by the
same gate.

## CR007 — AI stays in the app

**Date:** 2026-10-30  
**Status:** accepted

**Context:** The planner, prompts and RML generation (`lib/ai`, `feature/ai`) are bound to the shell's own UI
rules (`PromptBuilder`, the RML and RCSS profiles). Another app would not want them.

**Decision:** AI is part of the app, not a capability. Capabilities that need AI (messaging tools, structured
text) declare ports the app implements. Only generic tool / MCP plumbing may leave later, if an app asks.

## CR008 — Identity is people; the backend client is net; pricing is part of messaging

**Date:** 2026-10-30  
**Status:** accepted; amends DESIGN's identity (people + net + pricing); its "pricing is part of messaging" amended by CR020 (pricing and delivery sit below messaging and calls)

**Context:** The planned identity repo held three different things. People (keys, cards, contacts, devices,
the locker format) is identity. Net is the Brief backend client (registration, directory, blobs, push):
deployment-specific product code, against rule 1. Pricing is policy used by messaging (16 include sites)
and calls (5), and it needs messaging's thread and control types.

**Decision:** Identity = people (with `common/directory`). Net is its own capability above people. Pricing is
part of messaging.

## CR009 — Posts and content are one capability, below messaging

**Date:** 2026-10-30  
**Status:** accepted

**Context:** `lib/posts` depends only on crypto; `lib/content` (the content store) is used by posts and by
messaging's attachments. Neither had a place in the plan.

**Decision:** "Social" = posts + content, above people and below messaging. Posts never depends on messaging
(social-posts S029 keeps notices off the messaging path).

## CR010 — Wire contracts and vectors in their own repo

**Date:** 2026-10-30  
**Status:** accepted; not started

**Context:** Formats now have a C++ and a TypeScript implementation each (posts in app-posts, the locker and
the KEM binding in app-static-api), and vectors are copied by hand ("copy the file again when pp-browser
regenerates it").

**Decision:** Specs and golden vectors for formats implemented in more than one language move to their own
repo, which both sides pin and test against. Independent of capability moves.

## CR011 — Layers inside, capabilities across; the gate covers tests

**Date:** 2026-10-30  
**Status:** accepted

**Decision:** Both axes hold. Layers ([SRC_LAYOUT](../../docs/architecture/SRC_LAYOUT.md)) still order code
inside every capability, and a capability's contract DTOs under `common/<capability>/` belong to it
(`common/chat`, `common/thread` → messaging; `common/directory` → people). Across capabilities the order of
DESIGN § Target holds, checked by `scripts/check/check_capability_edges.py`, tests included: a capability's
tests move with it.

## CR012 — Before any repo moves: pins, vendored deps, visibility

**Date:** 2026-10-30  
**Status:** accepted

**Decision:**
- **Pin tooling first:** one pin list per app and a local mode that builds sibling checkouts together exist
  before the capabilities repo does (pp-ledger-apps' `PpFetchPin.cmake` is the starting point).
- **One home for vendored third-party code:** it stays committed (never fetched at configure), in one shared
  place rather than a copy per repo.
- **Public under people-post**, like the pp-cpp-* repos.
- The runtime core can go to pp-cpp-common any time (P2); the hard lab and pp-node packaging go with node.

## CR013 — Lib peers follow the capability order

**Date:** 2026-10-31  
**Status:** accepted; amends [SRC_LAYOUT § lib](../../docs/architecture/SRC_LAYOUT.md) ("strict independence")

**Context:** `SRC_LAYOUT` banned every `lib/` → `lib/` include and link, from before the capability order
existed. With the order gated (CR011), that ban also forbade the dependencies the order describes: account
cards and the peer key stores are identity (CR008), but `lib/messaging` uses them, so they could not live in
`lib/people`. The only homes left were `common/` (blocked by their `foundation/crypto` use) or a people
folder in `foundation/` (against rule 1), and helpers that compose people with messaging types had to bend
their APIs to stay out of both.

**Decision:** A lib peer may include and link a peer of a **lower capability** (messaging → people,
net → people, calls → messaging…). Two peers of **one** capability (`lib/calls` / `lib/media`,
`lib/messaging` / `lib/pricing`) and edges against the order stay banned. The layer checks
(`check_base_includes.sh`, `check_base_public_libs.sh`) take the allowed pairs from
`check_capability_edges.py --lib-edges`, so the order has one home.

**Rationale:** The capability gate already gives every cross-capability include a direction and fails on the
wrong one; the lib-peer ban added nothing on those edges but forced code into the wrong capability. Within a
capability the ban still keeps engines single-purpose.

**First use:** people → messaging cut from 21 sites to 0 — cards, key stores and `DevicePeerIdFromKey` into
`lib/people`; the people + thread compositions (`PeerBriefRoute`, `PeerDisplayResolver`,
`DirectChatTargetFromContact`, `ModelFacingJson`, `LocalKeysFromIdentity`, `GroupInviteStanding`) into
`lib/messaging`; `PeerDisplaySources` into `feature/conversations`, `PeopleDiscoveryContactAdapt` into
`feature/ai`.


## CR014 — The composition layer: capabilities declare, `feature/product` composes

**Date:** 2026-11-01  
**Status:** draft; describes the pattern P1's cuts used (PR #443)

**Context:** Three of P1's hardest cuts ended in `feature/product`: the people picker's ports
(`MessagingPeoplePickerPorts`), messaging's AI tools (`MessagingTools`) and the mesh (`ProductMesh`, lent to
the conversations hub through `HubMeshOwner`). The smaller ones followed the same shape: `ChatPresenter`
takes the preview line's prose from its binder instead of naming AI's parser. The design names the
capabilities and the order but not the place where they meet.

**Decision:** Rule 7: a capability declares what it needs from a peer as a port or an owner interface, in its
own vocabulary (V048); the composition layer (`feature/product`, then `app/` and `gui/`) builds the peers and
wires them. A capability never constructs another capability's root (a hub, a mesh, a stack).

**Consequences:** `feature/product` is the app's composition root below the GUI, and in the capability map
it is the app's. A second app writes its own composition; it does not reuse `feature/product`.

## CR015 — P2 is gated like P1: the product's base is the apps'

**Date:** 2026-11-01  
**Status:** draft; implementation started

**Context:** P1 worked because the gate counted sites and the allowlist only shrank. P2 ("capabilities stop
reading `Config`, `UserPreferences`, `MeshRole`, platform hooks, `Tr()`") was a checklist with no count.
Mapped as a group, the product's base starts at 7 edges / 85 sites: messaging 52, calls 20, people 5,
base 3, node 2, net 2, broadcast 1. Seventeen of them are pricing DTOs that only sit in `foundation/data`.

**Decision:** `foundation/data`, `foundation/platform` and `foundation/i18n` form the gate group **product**
(rule 1: config and its format, data paths, platform hooks, user-facing text). It may include the base; only
the apps (`app`, `node_app`) may include it. Today's sites go in the allowlist, which only shrinks.

**Consequences:** P2 has a number. Misplaced DTOs leave first; the rest is ports and values the app passes in
(CR014). Platform mechanisms a capability truly needs (time, secure storage) move to the base with a narrow
interface rather than staying behind the product's.

**First mechanism moved (2026-11-01, Hongwei):** the per-OS TLS trust for curl leaves `foundation/platform` for
its own base module, `foundation/tls` (`pp_foundation_tls`).

**The hub and calls take values (2026-11-01):** the conversations hub takes `HubSettings` (the mesh slice, the
endpoints, the initiation floor, the app's data directory, the profile's policies) instead of `AppConfig`, the
session store and the preferences file; `feature/product/HubSettingsProjection` projects them. The phone rule is
a value (`ConversationsHub::Host`, `EffectiveMeshConfig` / `MeshRoleOf` in `common/MeshConfig.h`): the hub
publishes the mesh config as it applies on the device, and calls read that. The seed merge
(`EffectiveBootstrapPeers`) is pure on the normalized config the app hands over. The Brief LLM key rotation takes
the service's URL from the app.

**Platform mechanisms (2026-11-01):** pure or POSIX mechanisms into the base (`common/ImageMetadata`,
`foundation/diagnostics/OsPeakRss`); the push registrar's interface and registry into `common/` (the platform
implements it); the attachment cache's quarantine mark and video posters behind `lib/messaging/AttachmentPlatform`,
which `ProductStack` installs; the profile icon is prepared by the app and uploaded by the hub; the ring sound's path
is handed to `CallRingtone`; broadcast takes the phone flag as a value; `CallSystemUiBridge` is a presenter
(`feature/presenters`, CR019).

**Text (2026-11-01):** capabilities and the base ask for user-facing text by catalog key through a port in the base
(`common/Localized.h`: `Localized(key, args)`); the app's `LocalizationService` registers as its translator once
its catalogs load, and aliases the shared platform-tip key to its platform's. The catalogs and the text stay the
app's. Still a step short of the plan's "error codes, text in the app": capabilities name keys and some strings
(call statuses, thread previews, call rows) are rendered at write time. Turning those into codes the app renders is
per-surface follow-up work. The config-secrets store moved to `foundation/data` (the app's config). With these,
the gate's product edges are all cut.

**Codes where they pay (2026-11-01):** call history rows are codes now (`CallSummaryRow::Outcome` and the
duration; the GUI says the words). Kept on keys, on purpose: thread previews (stored as text when written — a
code would change what the store holds, and old rows are text), and call statuses / errors (strings through
`SetMediaActivity` / `SetLastMediaError` and `CallMediaErrorText`'s mapping of raw errors; codes there are a
redesign of the call error path). With no capability depending on the app's i18n, the port is the boundary.

## CR016 — The gate covers links, not only includes

**Date:** 2026-11-01  
**Status:** draft; implementation started

**Context:** The include gate missed stale links: `pp_feature_conversations` linked `pp_lib_calls` and
`pp_lib_media` with no include left, and `pp_domain_peers` linked `pp_lib_messaging` until PR #443. A repo
split follows the link graph, so a link against the order is the same defect as an include.

**Decision:** `check_capability_edges.py` also reads the libraries the CMake files declare
(`pp_browser_add_*_library(... PUBLIC_LIBS ...)` and `target_link_libraries`), maps each target to its
folder's capability, and fails on a link against the order the same way it fails on an include.

## CR017 — pp-node is an app; the node capability is `domain/node`

**Date:** 2026-11-01  
**Status:** draft; implementation started

**Context:** The map put `app/node` (pp-node's main, bootstrap, status HTTP) in the node capability, so node
counted the config reads an app is meant to make: 9 of node's files read `Config`, most of them pp-node's.

**Decision:** `app/node` is its own group, **node_app**: an app that may include the product's base and the
capabilities it picks (people, net, node), and nothing above them. The node capability is `domain/node`.

## CR018 — The hub's shell thins before messaging can move

**Date:** 2026-11-01  
**Status:** draft; implementation started

**Context:** With every edge cut, the unit a second app would reuse is still `ConversationsHub`: ~3.5k
lines tied to `AppConfig`. It builds its own backend clients from URLs and runs mesh glue no messaging needs:
LAN mDNS, the OS network monitor, the address-disclosure policy, seed-reservation renewal, the mesh
directory cache.

**Decision:** The hub keeps messaging (stores, workflows, delivery, participants). The mesh glue joins the
mesh's owner (`ProductMesh`), which tells the hub what it needs through `HubMeshOwner`; the backend clients are
built by the composition (net's factory) and handed to the hub. Step by step, one concern per change.

**Step 1 (2026-11-01):** the OS network monitor is the mesh owner's: `HubMeshOwner::WatchNetwork` /
`StopWatchingNetwork`, called where the hub started and stopped its monitor (timing unchanged); the network's
value types (`NetworkTransport`, `NetworkState`, `NetworkChange`) are the base's (`common/NetworkChange.h`), so
the hub, `HubParticipant` and the calls stack no longer include the platform's monitor.

**Step 2 (2026-11-01):** LAN mDNS is the mesh owner's: `StartLanDiscovery` / `AdvertiseOnLan` /
`StopLanDiscovery`. The hub keeps the decisions — which discovered peers are contacts (their endpoints go to
delivery), and whether to announce at all (the node role, or a call on a phone).

**Step 3 (2026-11-01):** the mesh directory cache is the mesh owner's (`StartDirectoryCache` with the hub's
directory client and the config; its disk path from the app's data directory). The hub reads the nodes and is told
when they change. A fetch with no directory configured now fails and keeps the last nodes instead of clearing
them. The seed-reservation renewal stays with the hub's `MeshConnectivity` (the neutral mesh object participants
borrow) and moves when that does.

**Step 5 (2026-11-01):** the neutral mesh objects — `MeshConnectivity`, `MeshMediaRelay`, the hop policy feed —
and the seed-reservation renewal are the mesh owner's. The hub tells it where the mesh is in its life
(`SetMeshInputs`, `WireMeshObjects`, `DetachMeshObjects`, `BeginMeshStop` / `DropMediaRelayClient` /
`EndMeshStop`, `RefreshMeshView`, `StayReachable`, `ClearMeshObjects`), in the order it used to run them itself, and
reaches peers through `Connectivity()`. Calls and broadcast get the objects from `ProductMesh`, not the hub; the
hub names no media relay.

**Step 4 (2026-11-01):** the backend's clients are a delivery-level set (`lib/delivery/BackendClientSet`: the
relay's message and push-device clients, client compat, the directory, registration, blobs, the locker), built
from the config's endpoints, kept while an endpoint is unchanged, signing with the profile's identity. The
platform's default endpoints are the app's: `ProductStack` hands them to the hub (`SetBackendDefaults`).

## CR019 — Presenters are the app's

**Date:** 2026-11-01  
**Status:** accepted (Hongwei, 2026-11-01); implementation started

**Context:** `ChatPresenter` (the chats list's order, unread, pin, mute, archive and its buttons) and parts of
`InboxController` sit in messaging. They are UI decisions shaped like `CallPresenter`; they are why messaging
had to take the preview line's prose from its binder. Another app would bring its own list.

**Decision:** Presenters move to the app side (`gui/` or a presenter folder the app owns);
messaging exposes the facade they read. Calls' presenter follows the same rule.

**Done (2026-11-01):** `feature/presenters` (`pp_feature_presenters`, mapped to the app) holds `ChatPresenter`,
`CallPresenter` and the chats list's text (`SessionListText`, `SessionMenu`, `GroupEventText`) with their tests; no
feature module includes it (`check_feature_includes.sh`). `GroupMentions` stays messaging's (the facade uses it).

## CR020 — Secure delivery below messaging and realtime

**Date:** 2026-11-01  
**Status:** accepted (Hongwei, 2026-11-01); amends CR003 / CR008's split; implementation started

**Context:** Calls include messaging at 26 sites and broadcast at 8, all narrow: thread record types and the
store port (call history), `SendRelayOptions`, the pairwise PSK session store, pairwise fanout, system
controls, the E2E public session logic, `IDirectMessageClient`. What realtime rides is sessions, envelopes, the
control lane and the outbox — not chat threads, groups or the inbox.

**Decision:** A **delivery** capability under both messaging and realtime: pairwise sessions,
envelopes and their signing, the control lane, the outbox, the relay and Amp transports. Chat (threads,
groups, inbox, attachments, pricing) stays messaging. Call history reaches the thread store through a port.

**Consequences:** An app could take secure delivery without chat (a VPN client's control channel, pp-node
services). Costs a re-map and a few seams; worth settling before anything moves, since it changes what the
first capabilities repo holds.

**First cut (2026-11-01):** the gate maps **delivery** (`lib/delivery`, `common/chat`, `common/thread`,
`domain/delivery`) and **pricing** (`lib/pricing`, its own capability, since calls bill too) below messaging
and calls; calls and broadcast may no longer include messaging, and include it nowhere. Moved into
`lib/delivery`: the PSK and E2E-public session logic and stores, envelope signing and the wire / payload
codecs, the control lane's envelopes, `OutboundMessage`, `DeliveryNotice`, `PeerRequestAuth`,
`PeerBriefRoute`, `DirectChatTargetFromContact`, `TextSpans`, `ReactionTypes`, the relay's message client and
ports with their mocks, `LocalKeys`. Up into messaging: `PeerTransports` and the chat history transport
(`feature/conversations`), `AttachmentDownloadPolicy`; `PeopleDiscoveryBlocks` to `common/directory`.
`ControlLane` takes the direct-chat client as a getter, `PeerRouteBook` the sender's verification as a bool.
Tests that round-trip through messaging's SQLite store stay messaging's.

## CR021 — pp-browser's crypto formats are delivery's; pp-cpp-crypto takes only generic pieces

**Date:** 2026-11-01  
**Status:** accepted (Hongwei, 2026-11-01); amends P2's "crypto additions into pp-cpp-crypto"

**Context:** P2 planned to move "the crypto capabilities add" — message and attachment ciphers, the PSK session
store — into pp-cpp-crypto. Most of it is pp-browser's wire format (payload encoding, AAD, HKDF info strings, PSK
bundles, the KEM binding domain, `account:` IDs); pp-cpp-crypto's own rule keeps product wire formats in the apps,
and the ledger apps share it. Since CR020 those message formats are what the delivery capability defines.

**Decision:** pp-cpp-crypto takes the generic pieces only: base64url, BLAKE2b-256 and `ReplayWindow` (v1.1.0);
`foundation/crypto/CryptoUtil.h` names pp-cpp-crypto's helpers. The message formats move to `lib/delivery`
(`SessionKeyDeriver`, `CanonicalAad`, `PskBundleCodec`, `PskFingerprint`, `AutoKeyEstablishment`, `IPskSessionStore`,
`AttachmentContentCipher`) and `MediaKeyWrap` to `lib/calls`. The AEAD blob both messages and the at-rest store use
(`MessageCipher`, `EncryptedPayload`) stays in the base, with the identity formats people uses (`HybridKem`,
`KemKeyBinding`, `MlDsa`'s `account:` ID, recovery) and the profile's at-rest crypto (vault, PIN, `FileCipher`).


## CR022 — Capabilities move into `capabilities/<name>/` before any repo split

**Date:** 2026-11-01  
**Status:** accepted (Hongwei, 2026-11-01); implemented 2026-11-01 — all nine folders; current state in
[SRC_LAYOUT § Capability folders](../../docs/architecture/SRC_LAYOUT.md#capability-folders)

**Context:** P0–P2 made the capabilities clean in place, but they are still paths scattered under `src/`, held apart
by a script. Moving one into a repo (CR005 / CR006) would be a large change in one step, and pp-browser's CI runs
every capability's unit tests on every PR whatever it touched. `third_party/` was considered and rejected: it means
external code nobody edits, which the checks, formatting and lint skip.

**Decision:**
- Each capability gets a top-level folder, `capabilities/<name>/`, whose `src/` mirrors the tree it came from
  (`capabilities/people/src/lib/people/…`), so include paths do not change. Its `CMakeLists.txt` adds that `src/`
  as an include root of its own targets and is added from `src/CMakeLists.txt` before the libs that use it.
- The capability gate reads every capability folder too, and fails when a folder holds another capability's paths.
  Once every capability has moved out of `src/`, a capability's targets see only the include roots below it, and the
  compiler enforces the order as well.
- A capability's tests carry the CTest label `capability:<name>` (set once by the folder). CI's changes filter has an
  output per moved capability that is true when the folder or anything below it changes (the base, `cmake/`,
  `third_party/`); a PR whose filter is false skips those tests (`ctest -LE`). Pushes to develop and main run every
  test.
- A later split of a capability into the capabilities repo is a `git subtree split capabilities/<name>`.

**Order:** people, net, social, delivery, pricing, messaging, calls, broadcast, node — one change each, as in P1.

**Notes (implementation):**
- Feature modules linked the app's bundles (`pp_base`; their tests `pp_feature`) through the feature helper, which the
  gate, reading named links, did not see. In a capability folder they link only what they name (Hongwei chose this
  over moving them as they were). It surfaced one hidden edge, the hub's `SDL_GetTicks`, now `steady_clock`.
- `src/` stays an include root of every target, so an app path still compiles in a capability; the gate stops it.
  Capability roots come only through links, so the compiler stops a capability including one above or beside it.

## CR023 — Calls becomes repo-ready first

**Date:** 2026-11-02  
**Status:** accepted (Hongwei, 2026-11-02); step 1 done

**Context:** Every capability has its folder (CR022), but none yet behaves like a repo of its own: building,
testing and documenting itself with only what is below it. Calls is the first to try (measured 2026-11-02):
its includes are clean (people, delivery, pricing, the base, pp-cpp-*), but 18 of its composed tests lived in
the app's `feature/product/tests`, `lib/media` mixed four owners (devices, the call engine, voice messages, QR
scanning), it reached three app concepts in `foundation/runtime` (branding, lifecycle, `ProductOwners`), it
called the app's Android activity by method name, and `src/` (the app) stays an include root of its targets.

**Decision:** calls is made repo-ready in steps, one PR each:
1. Calls owns its tests: the call tests move into the calls folder, with delivery's in-memory stores in place
   of messaging's SQLite ones (CR024); tests that check the app's wording stay with the app.
2. `lib/media` splits by owner: devices and codecs into a `media` capability below calls (broadcast and voice
   messages use it too), voice messages to the app's messaging side, QR scanning to the app's scan feature.
3. The base gets its own folder, so `src/` stops being an include root of capabilities; calls takes branding,
   lifecycle and owners as values or ports.
4. A standalone build of calls with what is below it, and a CI job that builds and tests only that.
5. A host contract (audio route, call system UI, the Android hooks) as ports the app fills; calls gets a README
   and `docs/` (CALLS.md and its wire schemas).

Namespaces and an `include/` split wait for an actual repo split (CR022).

## CR024 — Capabilities own ideas; the app owns the database

**Date:** 2026-11-02  
**Status:** accepted (Hongwei, 2026-11-02); stages 1 and 2 done; stage 3 (calls) next

**Context:** Moving calls' tests out of the app (CR023 step 1) showed delivery's PSK store writing into
`chat_targets`, a table only messaging's schema creates: delivery could not keep a session key without
messaging's store having opened the database first. The gate does not see it — it is a table name in SQL, not
an include. Each capability also carried at-rest encryption (the profile DEK) inside its SQLite stores.

**Decision:**
- A capability ships its records, its logic and the ports it needs to keep them, and an in-memory implementation
  for tests — no SQL. The app owns the database file, every schema and migration, at-rest encryption, and the
  SQLite implementations of the capabilities' ports, in one storage module (`src/lib/storage`).
- Logic stays in the capability above a thin port: delivery's `IPeerSessionStore` keeps `Load` / `List` / `Save`
  / `AllocateSenderSeq` (atomic) as the port, and resolving an epoch's key, verifying, exporting and importing a
  key bundle on top of them, in delivery.
- Delivery's session idea is renamed for what it is: `ChatTargetKey` / `PskSessionRecord` / `IPskSessionStore`
  become `PeerSessionKey` / `PeerSessionRecord` / `IPeerSessionStore` (the key moves from `foundation/crypto` into
  delivery). The table follows when its schema moves into storage: `chat_targets` → `peer_sessions`.

**Stages:**
1. Delivery's ideas: the rename; the thin port with delivery's logic on top; a port for the control lane's seen
   ids; one in-memory store for delivery's ports (`InMemoryDeliveryStore`: the threads and the peer sessions over
   shared rows, as in SQLite) and in-memory seen ids. Calls takes the ports, and its tests run in memory.
2. Storage, in two commits. (a) Messaging's stores become ports in place — `IGroupRosterStore`,
   `IThreadLocalPrefsStore`, `IThreadMentionsStore` — with in-memory implementations (`lib/messaging/testing`; the
   roster shares each group's send target with `InMemoryDeliveryStore`, as SQLite's roster and thread store share
   `group_targets`) and a contract test that runs every case on both. (b) `src/lib/storage` takes the SQLite side
   together — the profile database and its schema, the thread store, rosters, thread prefs, mentions, the
   peer-session and control-lane stores (they share rows, so they move as one) — handed to the hub by the app;
   `chat_targets` is renamed `peer_sessions` there. Messaging's SQLite tests go with them; its logic tests run in
   memory.
3. Calls: `CallSessionStore` and `CallMediaKeyStore` become ports with in-memory implementations; the SQLite ones
   move to storage (calls drops its at-rest crypto includes).
4. People and social: `PeerCardStore`, `PostStore` and `ObjectIndex` the same way.

**Notes (stage 1):** the hub's two accessors for the session store became one, `PeerSessions()` (the port).
