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

Agent entry points: [`AGENTS.md`](../AGENTS.md) (orientation), this map (stable docs).

---

## Architecture & platform

| Doc | Topic |
|-----|--------|
| [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) | Overall system shape |
| [architecture/SRC_LAYOUT.md](architecture/SRC_LAYOUT.md) | North Star: `app → feature → domain → foundation → common` — feature/app cleanup: [projects/feature-layer-reorg/](../projects/feature-layer-reorg/) |
| [architecture/RUNTIME_COMPOSITION.md](architecture/RUNTIME_COMPOSITION.md) | App ↔ messaging / shell / chat / settings wiring + threads |
| [architecture/OWNERSHIP.md](architecture/OWNERSHIP.md) | Parent-only destroy (repo-wide); mesh detail [A027](../projects/adp/DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy) |
| [architecture/THREADING.md](architecture/THREADING.md) | Thread roles — coordinator, worker pool, `AppRuntime` |
| [architecture/UI_FUNCTIONAL_BOUNDARY.md](architecture/UI_FUNCTIONAL_BOUNDARY.md) | UI vs functional systems; state / config / actions / events; app-owned presenters |
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
| [contracts/AT_REST_ENCRYPTION.md](contracts/AT_REST_ENCRYPTION.md) | PIN vault, DEK, `identity.enc` | vault file version |
| [contracts/DATA_LAYOUT.md](contracts/DATA_LAYOUT.md) | Paths, profile tree, JSON schema versions | on-disk layout |
| [contracts/COMPATIBILITY.md](contracts/COMPATIBILITY.md) | Dirty folders; newer peer/API; wipe vs migrate | policy |
| [contracts/SERVICE_ENDPOINTS.md](contracts/SERVICE_ENDPOINTS.md) | HTTP relay / directory / registration | `/v1/…` surface |
| [contracts/MESH_DHT.md](contracts/MESH_DHT.md) | AMP Kademlia peer routing (n2 draft) | wire `version` 1, `protocol_id` `/pp-mesh/dht/1.0.0` |
| [contracts/MESH_DIRECTORY_AMP.md](contracts/MESH_DIRECTORY_AMP.md) | Amp directory twin of HTTP phone book (N029 nd4) | `/pp-mesh/directory/1.0.0`, Amp-first then HTTP |
| [contracts/ADP.md](contracts/ADP.md) | Association Datagram Protocol (UDP L1) | wire version `1` |
| [contracts/AMP-SESSION.md](contracts/AMP-SESSION.md) | AMP Session (L2 MSH + full AEAD) | `msh_version`, `session_epoch` |
| [contracts/AMP-CHANNEL.md](contracts/AMP-CHANNEL.md) | AMP Channel mux (L3); ownership hierarchy [A027] | `channel_frame_version`, `protocol_id` |
| [contracts/CODED_FAILURE.md](contracts/CODED_FAILURE.md) | Per-module `CodedFailure` escalation; adapter vs wrap; rollout | in-process `Err` ints (not wire) |
| [contracts/AMP-LINK-ERRORS.md](contracts/AMP-LINK-ERRORS.md) | Stable `PeerLinkManager` / `IChatPeerLinks` link `Err` table | link/port codes only |

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
| [ui/RML_PROFILE.md](ui/RML_PROFILE.md) / [ui/RCSS_PROFILE.md](ui/RCSS_PROFILE.md) | AI-safe RML/RCSS (agent conventions) |
| [ui/CHAT_TEMPLATES.md](ui/CHAT_TEMPLATES.md), [ui/INPUT.md](ui/INPUT.md), [ui/AGENT_CONVERSATION.md](ui/AGENT_CONVERSATION.md) | Chat UX / agent conversation |

---

## Ops / howto

| Doc | Topic |
|-----|--------|
| [ops/BUILD.md](ops/BUILD.md) | Build and test |
| [ops/TEST_STRATEGY.md](ops/TEST_STRATEGY.md) | Tiered testing; `N-*` / `B-*` purposes; inventory; CI ladder |
| [ops/RELEASE.md](ops/RELEASE.md) | Branching (`develop`/`main`), app `v*` vs `pp-node/v*` trains, artifacts |
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
5. Mark the project **Done / archived** in [`projects/README.md`](../projects/README.md) when delivery ends (folder may remain for ADR history).
6. Never mirror phase status into [`AGENTS.md`](../AGENTS.md).
