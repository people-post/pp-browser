# Documentation map

pp-browser docs are split by **stability and blast radius**, not by feature name. Feature delivery notes live under [`projects/`](../projects/).

| Tier | Folder | Change bar | Purpose |
|------|--------|------------|---------|
| **Architecture & platform** | [`architecture/`](architecture/) | Rare; wide consequences | How the app is shaped (layers, forks, platforms) |
| **Contracts** | [`contracts/`](contracts/) | Versioned; additive preferred | Interop and on-disk formats that create historical debt |
| **Product / UI** | [`ui/`](ui/) | Evolve with product | Shell, theme, RML/RCSS profiles — stable enough for agents, not wire-hard |
| **Ops / howto** | [`ops/`](ops/) | Freely | Build, release, configuration howto |

**Active feature work:** [`projects/README.md`](../projects/README.md) — DESIGN → PHASES → CURRENT_STATE → DECISIONS; promote normative outcomes into **contracts/** when shipped.

Agent entry points: [`AGENTS.md`](../AGENTS.md), this map.

---

## Architecture & platform

| Doc | Topic |
|-----|--------|
| [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) | Overall system shape |
| [architecture/SRC_LAYOUT.md](architecture/SRC_LAYOUT.md) | `app → feature → base → common` |
| [architecture/PLATFORMS.md](architecture/PLATFORMS.md) | Desktop / Android / path providers |
| [architecture/PLATFORM_CODE.md](architecture/PLATFORM_CODE.md) | OS code layout, `#ifdef` policy |
| [architecture/P2P_MESSAGING.md](architecture/P2P_MESSAGING.md) | Messaging architecture (pointers to wire/crypto) |
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

Configuration howto (Me tab, presets, env): [ops/CONFIGURATION.md](ops/CONFIGURATION.md).

---

## Product / UI conventions

| Doc | Topic |
|-----|--------|
| [ui/UI_DESIGN_SYSTEM.md](ui/UI_DESIGN_SYSTEM.md) | Tokens, components, theme |
| [ui/PRODUCT_BRANDING.md](ui/PRODUCT_BRANDING.md) | Product name (Frame), icon rationale, asset paths |
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
| [ops/RELEASE.md](ops/RELEASE.md) | Tagging, artifacts, release CI |
| [ops/MACOS_SIGNING.md](ops/MACOS_SIGNING.md) | Apple Developer ID — sign + notarize Frame.app |
| [ops/IOS_BUILD.md](ops/IOS_BUILD.md) | iOS simulator/device build + provisioning placeholders |
| [ops/CONFIGURATION.md](ops/CONFIGURATION.md) | Config resolution, Me tab, presets, env vars |

---

## Promotion rule (contracts)

1. Explore and decide in `projects/<name>/` (`DESIGN`, `DECISIONS`).
2. Ship behind `PHASES`; keep `CURRENT_STATE` accurate.
3. When a wire/disk/HTTP shape is **shipped**, promote the normative text into **`docs/contracts/`** in the same release window.
4. Mark project ADRs **superseded by** the contract doc for outcomes; leave rationale in `DECISIONS.md`.
5. Archive or delete the project folder when delivery ends — see [`projects/README.md`](../projects/README.md).
