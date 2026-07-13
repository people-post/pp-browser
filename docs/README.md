# Documentation map

pp-browser docs are split by **stability and blast radius**, not by feature name. Feature delivery notes live under [`projects/`](../projects/).

| Tier | Change bar | Purpose |
|------|------------|---------|
| **Architecture & platform** | Rare; wide consequences | How the app is shaped (layers, forks, platforms) |
| **Contracts** | Versioned; additive preferred | Interop and on-disk formats that create historical debt |
| **Product / UI conventions** | Evolve with product | Shell, theme, RML/RCSS profiles — stable enough for agents, not wire-hard |
| **Ops / howto** | Freely | Build, release, day-to-day paths |

**Active feature work:** [`projects/README.md`](../projects/README.md) — DESIGN → PHASES → CURRENT_STATE → DECISIONS; promote normative outcomes into **Contracts** when shipped.

Agent entry points: [`AGENTS.md`](../AGENTS.md), this map.

---

## Architecture & platform

| Doc | Topic |
|-----|--------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Overall system shape |
| [SRC_LAYOUT.md](SRC_LAYOUT.md) | `app → feature → base → common` |
| [PLATFORMS.md](PLATFORMS.md) | Desktop / Android / path providers |
| [RMLUI_UPSTREAM.md](RMLUI_UPSTREAM.md) | In-tree RmlUi fork deltas |
| [LIBP2P_UPSTREAM.md](LIBP2P_UPSTREAM.md) | In-tree libp2p fork deltas |

---

## Contracts (normative)

Shapes that peers, relay, older clients, or last year’s disk must understand. Prefer **one canonical file** per concern; projects link here after promotion — do not keep a second editable copy.

| Doc | Topic | Version axes |
|-----|--------|--------------|
| [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) | Relay envelope, ChatPayload, history wire | `envelope_version`, `payload_version`, protocol ids |
| [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md) | E2E AEAD, AAD, signing | `aad_version`, crypto blob version |
| [AT_REST_ENCRYPTION.md](AT_REST_ENCRYPTION.md) | PIN vault, DEK, `identity.enc` | vault file version |
| [CONFIGURATION.md](CONFIGURATION.md) | Paths, profile layout, JSON `schema_version` / `config_version` | on-disk layout |
| [COMPATIBILITY.md](COMPATIBILITY.md) | Dirty folders; newer peer/API; wipe vs migrate; soft vs hard reject | policy (links contracts) |
| [SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md) | HTTP relay / directory / registration | `/v1/…` surface |
| [P2P_MESSAGING.md](P2P_MESSAGING.md) | Messaging architecture + pointers to wire/crypto | — |

---

## Product / UI conventions

| Doc | Topic |
|-----|--------|
| [UI_DESIGN_SYSTEM.md](UI_DESIGN_SYSTEM.md) | Tokens, components, theme |
| [WINDOW_SHELL.md](WINDOW_SHELL.md) | Shell layout |
| [WORKING_SET_PANEL.md](WORKING_SET_PANEL.md) | Auxiliary pane |
| [RML_PROFILE.md](RML_PROFILE.md) / [RCSS_PROFILE.md](RCSS_PROFILE.md) | AI-safe RML/RCSS |
| [CHAT_TEMPLATES.md](CHAT_TEMPLATES.md), [AGENT_CONVERSATION.md](AGENT_CONVERSATION.md), [INPUT.md](INPUT.md) | Chat UX / agent conversation |

---

## Ops / howto

| Doc | Topic |
|-----|--------|
| [BUILD.md](BUILD.md) | Build and test |
| [RELEASE.md](RELEASE.md) | Release notes process |

---

## Promotion rule (contracts)

1. Explore and decide in `projects/<name>/` (`DESIGN`, `DECISIONS`).
2. Ship behind `PHASES`; keep `CURRENT_STATE` accurate.
3. When a wire/disk/HTTP shape is **shipped**, promote the normative text into **Contracts** (this directory) in the same release window.
4. Mark project ADRs **superseded by** the contract doc for outcomes; leave rationale in `DECISIONS.md`.
5. Archive or delete the project folder when delivery ends — see [`projects/README.md`](../projects/README.md).
