# Documentation map

pp-browser docs are split by **stability and blast radius**, not by feature name. Feature delivery notes live under [`projects/`](../projects/).

| Tier | Folder | Change bar | Purpose |
|------|--------|------------|---------|
| **Architecture & platform** | [`architecture/`](architecture/) | Rare; wide consequences | How the app is shaped (layers, forks, platforms) |
| **Contracts** | [`contracts/`](contracts/) | Versioned; additive preferred | Interop and on-disk formats that create historical debt |
| **Product / UI** | [`ui/`](ui/) | Evolve with product | Shell, theme, RML/RCSS profiles — stable enough for agents, not wire-hard |
| **Ops / howto** | [`ops/`](ops/) | Freely | Build, release, configuration howto |

**`docs/` = what the system is. `projects/` = what we are changing.** Phase status lives only in each project’s `CURRENT_STATE.md` — not in [`AGENTS.md`](../AGENTS.md).

**Active feature work:** [`projects/README.md`](../projects/README.md) — DESIGN → PHASES → CURRENT_STATE → DECISIONS; promote normative outcomes into **contracts/** (or the matching tier) when shipped.

Agent entry points: [`AGENTS.md`](../AGENTS.md) (orientation), this map (stable docs), [§ Where docs live](#where-docs-live) (which repo holds what).

---

## Where docs live

Decided in [CR025](../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs) (2026-11-23), in three steps: step 1 is done, step 2 comes after release 1, step 3 when a second app needs a capability. [CR026](../projects/capability-repos/DECISIONS.md#cr026--pp-cpp-amp-takes-the-stacks-design-and-the-contracts-it-alone-implements) moved the contracts only one library implements, and adp's library half, ahead of step 2.

1. **A doc lives with what it describes:** the repo of the code, or of the contract.
2. **Anything that spans repos has one neutral home**, and everything else links there.
3. **Decision ids never change.** RC020 stays RC020 wherever its file lives. Code cites ids (`thread-ownership T001`), not paths.

Specs and designs are public. Plans and infrastructure details are private, as [AGENTS.md § Documentation authority](../AGENTS.md#documentation-authority) draws the line for the relay's deployment.

### The organisation's repositories

| Repository | Visibility | Its docs |
|------------|------------|----------|
| **pp-browser** (this one) | public | The app: architecture, ops, UI ([tiers above](#documentation-map)), app-local contracts ([DATA_LAYOUT](contracts/DATA_LAYOUT.md), [AT_REST_ENCRYPTION](contracts/AT_REST_ENCRYPTION.md)), app-only projects, a `capabilities/<name>/docs/` per capability. Until step 2 it also holds the shared contracts, the vector files and the cross-repo programs (next table). |
| **pp-specs** | public; not created yet (step 2) | Contracts more than one repo implements, every `*_vectors.json` (tagged, pinned by each implementation), cross-repo programs |
| **pp-plan** | private | Roadmap, priorities and to-dos, with the organisation's project board #3. Nothing technical: whatever code relies on stays public. |
| [pp-cpp-common](https://github.com/people-post/pp-cpp-common) | public | README: the shared C++ base, the runtime core ([CR002](../projects/capability-repos/DECISIONS.md#cr002--runtime-core-goes-to-pp-cpp-common)); writes `ext_area_vectors.json` |
| [pp-cpp-crypto](https://github.com/people-post/pp-cpp-crypto) | public | README: shared crypto and PeerId; writes `alg_id_vectors.json` |
| [pp-cpp-amp](https://github.com/people-post/pp-cpp-amp) | public | `docs/` ([index](https://github.com/people-post/pp-cpp-amp/blob/main/README.md#docs)): the stack's big picture and layer map, its contracts (ADP, AMP-SESSION, AMP-CHANNEL's L3), design, decisions (adp's A001–A029 that are the transport's), test matrices, status (CR026); ownership, keepalive, tuning, link-plane ADRs, fault and performance cases |
| [pp-cpp-mesh](https://github.com/people-post/pp-cpp-mesh) | public | `docs/`: DHT, directory, errors, peer reach, threading; `docs/contracts/MESH_DIRECTORY_AMP.md` (CR026); `docs/projects/media-hop-reachability/` (moved in step 1) |
| [pp-cpp-ui](https://github.com/people-post/pp-cpp-ui) | public | `AGENTS.md`, `docs/`: the engine's layout ADRs, source layout, upstream provenance |
| [pp-ledger](https://github.com/people-post/pp-ledger) | public | Its own `docs/` (architecture, contracts, ops, product) and `projects/` |
| [pp-ledger-apps](https://github.com/people-post/pp-ledger-apps) | public | `docs/` (release, reproducible builds, guides) and `projects/` (one per app) |
| pp-relay-apis | private | The relay: `SPECS.md` and `openapi/relay.yaml` (what each route does), `docs/` (architecture, conventions, local stack, operators), its server designs, the relay's deployment |
| pp-relay-support-site | private | The support staff's console for the relay's admin API |
| The infrastructure repositories | private | The relay's, the chain's and the management account's infrastructure, and the infrastructure halves of the cross-repo programs. Not named in this public repo; pp-relay-apis' README lists the relay's. |
| [pp-js-lib](https://github.com/people-post/pp-js-lib), [people-post.github.io](https://github.com/people-post/people-post.github.io) | public | A JS library; the homepage |
| pp-js-api, pp-js-app, pp-js-main, pp-js-node | public, archived | The retired JS stack; frozen |

### Cross-repo things: today and in step 2

| What | Today | Step 2 (after release 1) |
|------|-------|--------------------------|
| Contracts another repo implements too: [SERVICE_ENDPOINTS](contracts/SERVICE_ENDPOINTS.md), [WIRE_SCHEMAS](contracts/WIRE_SCHEMAS.md), [ACCOUNT_CARD](contracts/ACCOUNT_CARD.md), [ACCOUNT_LOCKER](contracts/ACCOUNT_LOCKER.md), [SIGNED_EXTENSIONS](contracts/SIGNED_EXTENSIONS.md), [MESSAGE_ENCRYPTION](contracts/MESSAGE_ENCRYPTION.md) (the relay implements each) | [`contracts/`](contracts/) here | pp-specs, a stub here per file |
| Contracts one library alone implements: [ADP](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/ADP.md), [AMP-SESSION](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-SESSION.md), [AMP-CHANNEL's L3](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md) (pp-cpp-amp), [MESH_DIRECTORY_AMP](https://github.com/people-post/pp-cpp-mesh/blob/main/docs/contracts/MESH_DIRECTORY_AMP.md) (pp-cpp-mesh) | Moved to that library ([CR026](../projects/capability-repos/DECISIONS.md#cr026--pp-cpp-amp-takes-the-stacks-design-and-the-contracts-it-alone-implements)), a stub here per file | — |
| Contracts the libraries implement with the app or each other: AMP-LINK-ERRORS (pp-cpp-mesh mirrors amp's table), [AMP-CHANNEL](contracts/AMP-CHANNEL.md)'s protocols on channels, MESH_DHT, L4_PROTOCOL_KINDS, MEDIA_CHANNELS, CODED_FAILURE, NODE_METRICS | [`contracts/`](contracts/) here | pp-specs when more than one repo implements one, else the library that does (rule 1); settled per file in step 2 |
| Vector files (`*_vectors.json`, [SIGNED_EXTENSIONS § Vector files](contracts/SIGNED_EXTENSIONS.md#vector-files)) | [pp-specs](https://github.com/people-post/pp-specs/blob/main/vectors/README.md) `vectors/` (17 files, tag `v1.0.0` and later), read by the tests at the tag `cmake/PpPins.cmake` pins; the relay and the libraries still keep copies until they pin it | pp-specs, tagged; every implementation pins a tag instead of copying |
| Cross-repo programs: [release-compatibility](../projects/release-compatibility/), [multi-relay](../projects/multi-relay/), [support-chat](../projects/support-chat/), [push-notifications](../projects/push-notifications/), [relay-blob-upload](../projects/relay-blob-upload/), [capability-repos](../projects/capability-repos/) | `projects/` here (their server sides in pp-relay-apis) | pp-specs; their infrastructure halves to the infrastructure repositories |
| Library projects | [media-hop-reachability](https://github.com/people-post/pp-cpp-mesh/tree/main/docs/projects/media-hop-reachability) in pp-cpp-mesh (step 1, a stub here per file). adp's library half (stack, design, transport decisions, test matrices, status) in [pp-cpp-amp `docs/`](https://github.com/people-post/pp-cpp-amp/blob/main/README.md#docs) ([CR026](../projects/capability-repos/DECISIONS.md#cr026--pp-cpp-amp-takes-the-stacks-design-and-the-contracts-it-alone-implements)); its app half ([adp](../projects/adp/): the cutover, the app's decisions) and [p2p-mesh](../projects/p2p-mesh/) stay here | — |
| A capability's docs | `capabilities/<name>/docs/README.md` (what it is, its paths, the contracts and projects about it) | Stay; they leave with the capability (step 3, [CR005](../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)) |
| Finished projects | [`projects/_archive/`](../projects/_archive/README.md) | — |

### Moving a doc

- **Across repos:** copy it, with a header line in the copy ("Moved from pp-browser `<path>` at `<commit>`"), and replace the old file with a one-line stub naming the new URL, so every old link (other repos', code comments') keeps working. Links back into this repo become absolute `https://github.com/people-post/pp-browser/blob/main/…` URLs.
- **Inside one repo:** `git mv` and fix every link, code comments included; no stubs.
- Ids and headings stay as they were, so anchors survive the move.

---

## Architecture & platform

| Doc | Topic |
|-----|--------|
| [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) | Overall system shape |
| [architecture/SRC_LAYOUT.md](architecture/SRC_LAYOUT.md) | North Star: `app → feature → domain → lib → foundation → common` — feature/app cleanup: [projects/feature-layer-reorg/](../projects/feature-layer-reorg/) |
| [architecture/TESTING.md](architecture/TESTING.md) | Testing doctrine: tiers, push-down seams, promote failures downward, skip taxonomy, doc homes |
| [architecture/RUNTIME_COMPOSITION.md](architecture/RUNTIME_COMPOSITION.md) | App ↔ messaging / shell / chat / settings wiring + threads |
| [architecture/OWNERSHIP.md](architecture/OWNERSHIP.md) | Parent-only destroy (repo-wide); mesh detail [A027](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy) |
| [architecture/COMPOSITION_VOCABULARY.md](architecture/COMPOSITION_VOCABULARY.md) | No upward concepts through ports (repo-wide); calls [V048](../projects/p2p-av-calls/DECISIONS.md#v048--composition-vocabulary-no-upward-concepts) |
| [architecture/THREADING.md](architecture/THREADING.md) | Thread roles — coordinator, worker pool, `AppRuntime` |
| [architecture/LOGGING.md](architecture/LOGGING.md) | Logger obtain rules — Module / static `InitLogging` / free-fn `Logger&` |
| [architecture/UI_FUNCTIONAL_BOUNDARY.md](architecture/UI_FUNCTIONAL_BOUNDARY.md) | UI vs functional systems; state / config / actions / events; app-owned presenters |
| [architecture/SERVICE_CAPABILITIES.md](architecture/SERVICE_CAPABILITIES.md) | Coarse readiness flags; early UI vs async mesh; port/service gating |
| [architecture/PLATFORMS.md](architecture/PLATFORMS.md) | Desktop / Android / path providers |
| [architecture/PLATFORM_CODE.md](architecture/PLATFORM_CODE.md) | OS code layout, `#ifdef` policy |
| [architecture/P2P_MESSAGING.md](architecture/P2P_MESSAGING.md) | Messaging architecture (pointers to wire/crypto) |
| [architecture/NETWORKING.md](architecture/NETWORKING.md) | **HTTP + Amp mesh** doctrine; settle rails; no WebRTC product path |
| [architecture/MESH.md](architecture/MESH.md) | Mesh layer organization (Amp + PeerId) |
| [architecture/MESH_IDENTITY.md](architecture/MESH_IDENTITY.md) | Mesh identity binding |
| [architecture/LIBP2P_STREAMS.md](architecture/LIBP2P_STREAMS.md) | Stream framing, exchanges, size/hang handling |
| [architecture/CALLS.md](architecture/CALLS.md) | A/V call **code** map; product work in `projects/p2p-av-calls/` |
| [architecture/RMLUI_UPSTREAM.md](architecture/RMLUI_UPSTREAM.md) | In-tree RmlUi fork deltas |
| [architecture/LIBP2P_UPSTREAM.md](architecture/LIBP2P_UPSTREAM.md) | In-tree libp2p fork deltas |

---

## Contracts (normative)

Shapes that peers, relay, older clients, or last year’s disk must understand. Prefer **one canonical file** per concern; projects link here after promotion — do not keep a second editable copy.

| Doc | Topic | Version axes |
|-----|--------|--------------|
| [contracts/WIRE_SCHEMAS.md](contracts/WIRE_SCHEMAS.md) | Relay envelope, ChatPayload, history wire | `envelope_version`, `payload_version`, protocol ids |
| [contracts/MESSAGE_ENCRYPTION.md](contracts/MESSAGE_ENCRYPTION.md) | E2E AEAD, AAD, signing | `aad_version`, crypto blob version |
| [contracts/ACCOUNT_CARD.md](contracts/ACCOUNT_CARD.md) | Account cards: keys and devices that prove themselves; locator; card exchange over Amp | `card_version`, `pp-browser-contact-v1:`, `/pp/account-card/1.0.0` |
| [contracts/LINK_DEVICE.md](contracts/LINK_DEVICE.md) | Linking a device: the sealed QR handshake (the new device's one-time key, the main device's sealed seed, the 4 digits) | link offer v1, link reply v1 (`pp-browser-link-offer:` / `pp-browser-link-reply:`) |
| [contracts/ACCOUNT_LOCKER.md](contracts/ACCOUNT_LOCKER.md) | Account locker: recovery-seed keys, sealed snapshot, state JSON, sync between devices, restore | `pp-browser:account-locker` (record v2), `account_locker.json` |
| [contracts/AT_REST_ENCRYPTION.md](contracts/AT_REST_ENCRYPTION.md) | PIN vault, DEK, `identity.enc` | vault file version |
| [contracts/DATA_LAYOUT.md](contracts/DATA_LAYOUT.md) | Paths, profile tree, JSON schema versions | on-disk layout |
| [contracts/COMPATIBILITY.md](contracts/COMPATIBILITY.md) | Dirty folders; newer peer/API; wipe vs migrate | policy |
| [contracts/SIGNED_EXTENSIONS.md](contracts/SIGNED_EXTENSIONS.md) | Every signed object's shared rules: outside/inside version, signed-bytes header, the extension area (`ext_b64`), algorithm ids and typed blobs, the receiver's order; the vector files | each object's `*_version`; extension types, algorithm ids (registries) |
| [contracts/SERVICE_ENDPOINTS.md](contracts/SERVICE_ENDPOINTS.md) | HTTP relay / directory / registration | `/v1/…` surface |
| [contracts/MESH_DHT.md](contracts/MESH_DHT.md) | AMP Kademlia peer routing (n2 draft) | wire `version` 1, `protocol_id` `/pp-mesh/dht/1.0.0` |
| pp-cpp-mesh [MESH_DIRECTORY_AMP.md](https://github.com/people-post/pp-cpp-mesh/blob/main/docs/contracts/MESH_DIRECTORY_AMP.md) | Amp directory twin of HTTP phone book (N029 nd4) | `/pp-mesh/directory/1.0.0`, Amp-first then HTTP |
| pp-cpp-amp [ADP.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/ADP.md) | Association Datagram Protocol (UDP L1) | wire version `1` |
| pp-cpp-amp [AMP-SESSION.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-SESSION.md) | AMP Session (L2 MSH + full AEAD) | `msh_version`, `session_epoch` |
| pp-cpp-amp [AMP-CHANNEL.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md) | AMP Channel mux (L3); ownership hierarchy [A027] | `channel_frame_version`, `protocol_id` |
| [contracts/AMP-CHANNEL.md](contracts/AMP-CHANNEL.md) | The protocols on channels: `protocol_id` map, call-media bundle (path migration, failover, `bye`), circuit tunnel requests | `protocol_id`, message `v` |
| [contracts/L4_PROTOCOL_KINDS.md](contracts/L4_PROTOCOL_KINDS.md) | L4 conversation kinds + gate for new `protocol_id`s ([A028](../projects/adp/DECISIONS.md#a028--l4-protocol-kinds--seven-conversation-shapes) / [N030](../projects/p2p-mesh/DECISIONS.md#n030--adopt-l4-protocol-kinds-gate)) | kind taxonomy (wire ids versioned separately) |
| [contracts/CODED_FAILURE.md](contracts/CODED_FAILURE.md) | Per-module `CodedFailure` escalation; adapter vs wrap; rollout | in-process `Err` ints (not wire) |
| [contracts/AMP-LINK-ERRORS.md](contracts/AMP-LINK-ERRORS.md) | Stable `PeerLinkManager` / `IChatPeerLinks` link `Err` table | link/port codes only |
| [contracts/MEDIA_CHANNELS.md](contracts/MEDIA_CHANNELS.md) | Media frame `channel_id`: track kind + video level; relay QoS type; level negotiation | layout (one byte: kind / level) |
| [contracts/DIAGNOSTICS.md](contracts/DIAGNOSTICS.md) | What may leave the device: shareable lines (`Diag`, `Metrics`), the crash record, Send run logs (test phase: the archive, the pre-filter) | `schema_version` 1; event names / keys |
| [contracts/NODE_METRICS.md](contracts/NODE_METRICS.md) | pp-node `/metrics` (Prometheus text): series names, types, labels | series names (stable) |

Configuration howto (Me tab, presets, env): [ops/CONFIGURATION.md](ops/CONFIGURATION.md).

---

## Product / UI conventions

| Doc | Topic |
|-----|--------|
| [ui/UI_DESIGN_SYSTEM.md](ui/UI_DESIGN_SYSTEM.md) | Tokens, components, theme |
| [ui/PRODUCT_BRANDING.md](ui/PRODUCT_BRANDING.md) | Product name (PP), icon asset paths, naming rule |
| [ui/WINDOW_SHELL.md](ui/WINDOW_SHELL.md) | Shell layout |
| [ui/shell_layout_review.html](ui/shell_layout_review.html) | Static HTML mock — expanded vs compact pages (team UI review) |
| [ui/WORKING_SET_PANEL.md](ui/WORKING_SET_PANEL.md) | Auxiliary pane |
| [ui/SCANNING.md](ui/SCANNING.md) | In-app QR scan, friend confirmation and extension ports |
| [ui/RML_PROFILE.md](ui/RML_PROFILE.md) / [ui/RCSS_PROFILE.md](ui/RCSS_PROFILE.md) | AI-safe RML/RCSS (agent conventions) |
| [ui/CHAT_TEMPLATES.md](ui/CHAT_TEMPLATES.md), [ui/INPUT.md](ui/INPUT.md), [ui/AGENT_CONVERSATION.md](ui/AGENT_CONVERSATION.md) | Chat UX / agent conversation |

---

## Ops / howto

| Doc | Topic |
|-----|--------|
| [ops/BUILD.md](ops/BUILD.md) | Build and test |
| [../scripts/README.md](../scripts/README.md) | Repo scripts layout (`check/` `platform/` `vendor/` `test/` `dev/`) |
| [ops/TEST_STRATEGY.md](ops/TEST_STRATEGY.md) | Purpose IDs (`N-*` / `B-*` / `N-HARD-*`), inventory, CI ladder, SQLite test conventions — doctrine: [TESTING.md](architecture/TESTING.md); hard lab: [HARD_LAB.md](../packaging/pp-node/HARD_LAB.md) |
| [ops/RELEASE.md](ops/RELEASE.md) | Branching (`main` only), app `v*` vs `pp-node/v*` trains, artifacts |
| [ops/runbooks/](ops/runbooks/README.md) | Runbooks: release, hotfix, app incident (the relay's live in pp-relay-apis) |
| [ops/MACOS_SIGNING.md](ops/MACOS_SIGNING.md) | Apple Developer ID — sign + notarize PP.app |
| [ops/IOS_BUILD.md](ops/IOS_BUILD.md) | iOS simulator/device build + provisioning placeholders |
| [ops/APP_STORE_EXPORT_COMPLIANCE.md](ops/APP_STORE_EXPORT_COMPLIANCE.md) | App Store Connect encryption questionnaire + crypto inventory |
| [ops/CONFIGURATION.md](ops/CONFIGURATION.md) | Config resolution, Me tab, presets, env vars |

---

## Promotion rule (contracts)

1. Explore and decide in `projects/<name>/` (`DESIGN`, `DECISIONS`).
2. Ship behind `PHASES`; keep `CURRENT_STATE` accurate.
3. When a wire/disk/HTTP shape is **shipped**, promote the normative text into **`docs/contracts/`** in the same release window.
4. Mark project ADRs **superseded by** the contract doc for outcomes; leave rationale in `DECISIONS.md`.
5. When delivery ends, `git mv` the project to [`projects/_archive/`](../projects/_archive/README.md), fix its links, and list it under **Done / archived** in [`projects/README.md`](../projects/README.md) (CR025; its ids stay valid).
6. Never mirror phase status into [`AGENTS.md`](../AGENTS.md).
