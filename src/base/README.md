# `src/base`

Product-specific **primitives** for pp-browser: stores, clients, codecs, and UI building blocks that feature code composes into screens and workflows.

```
app        wires startup, profiles, global services
feature    chat, shell, agent session, messaging hub
base       ← you are here
lib        owned hard forks (rmlui, libp2p)
common     logger, ResultOrError, task runner (app-agnostic)
```

**Rule:** dependencies flow downward only. Base may use `lib/` and `common/`; it must not `#include` from `feature/` or `app/`. Repo-wide layout: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).

Each top-level folder (and `ai/conversation`, `ai/mcp`) builds as its own static library — **`pp_base_<module>`** (e.g. `pp_base_data`, `pp_base_ai_conversation`). The aggregate **`pp_base`** (`INTERFACE`; alias `pp_identity`) links all module libraries for feature and app code. See [`CMakeLists.txt`](CMakeLists.txt) and per-folder `CMakeLists.txt` files.

---

## What belongs here

Base code should be **single-purpose**: one store, one client, one parser, one codec.

| Put it in base when… | Put it in feature when… |
|----------------------|---------------------------|
| It owns a data model or wire format | It coordinates several base modules into a user flow |
| It talks to one external system (HTTP, SQLite, libsodium) | It implements a screen, controller, or session lifecycle |
| It is reusable across multiple features | It is only meaningful in one UI context |

If you are unsure, ask: *“Could another feature import this without pulling in a specific screen?”* Yes → base. No → feature.

---

## Module map

Eleven top-level folders (plus `ai/` sub-trees). Forks live under `src/lib/`.

```
src/base/
├── runtime/      Process runtime — AppRuntime, coordinator, lifecycle, branding/version
├── platform/     OS adapters — SDL glue, paths, assets, credentials, notifications
├── data/         Config, profiles, session, schema version, atomic file writes
├── error/        App error categories on top of common/Error.h
├── i18n/         Localization catalogs (JSON assets)
│
├── crypto/       E2E message crypto, hybrid KEM, at-rest PIN vault
├── p2p/          Libp2p product glue — host, mesh, circuit/media relay, streams
├── people/       Identity, contacts, Ed25519 signing
├── net/          HTTP client, relay / registration / directory clients
├── messaging/    Threads, SQLite + JSON stores, relay/group/E2E codecs
├── media/        Call capture/playback + HW H264
│
├── ai/           LLM client, turn plans, structured chat parsing, MCP
│   ├── conversation/   transcript, context policies, compaction
│   └── mcp/            MCP client, runtime, schema adapter
├── ui/           Theme, view catalog, chat widget DTOs/form helpers, input glue
└── render/       RmlUi SDL/GL backend (pp_base_render)
```

**Domain grouping (mental model):**

| Domain | Modules | Typical question it answers |
|--------|---------|----------------------------|
| **Runtime** | platform, data, error, i18n | Where do files live? What is configured? How do we report errors? |
| **Identity & trust** | people, crypto | Who am I? How are messages and disk encrypted? |
| **Connectivity** | p2p, net, messaging, media | How do we reach peers and services? How are threads/calls carried? |
| **Intelligence & presentation** | ai, ui, render | LLM/context? Shell types? SDL/GL RmlUi backend? |

Start points when exploring:

- Bootstrap & config → `data/BootstrapTypes.h`, `data/Config.h`
- Chat persistence → `messaging/IThreadStore.h`, `messaging/SqliteThreadStore.h`
- Agent transcript → `ai/conversation/Conversation.h`
- Shell theming → `ui/Theme.h`, `ui/ViewCatalog.h`

Includes use the repo root: `#include "base/data/Config.h"`.

---

## Dependency design

**Goal:** small modules that are as independent as possible, arranged in a **hierarchy without cycles**. Shared types live in the module that owns the data or protocol, not in a consumer.

Intended direction:

```
common
  ↑
platform, i18n
  ↑
error (uses i18n for catalogued messages)
  ↑
data (includes PlatformDefaults — config defaults keyed by PlatformKind)
  ↑
crypto
  ↑
p2p
  ↑
people
  ↑
net
  ↑
messaging
  ↑
ai, ui, render
```

Resolved cycles (2026): `PlatformDefaults` moved from `platform/` to `data/`; contact JSON helpers live in `people/ContactJson.*`; envelope PSK resolution lives in `messaging/AutoKeyEnvelopeResolver.*`.

**Principles for new code:**

1. **Downward includes only** — when module A needs a type from B, B should not include A’s headers.
2. **Shared structs go low** — if two modules need the same DTO, move it to the lower owner (or a dedicated `*Types.h` in that owner).
3. **Headers are contracts** — prefer heavy includes in `.cpp` files; keep headers lean to limit compile-time coupling.
4. **Orchestration stays up** — multi-module workflows (`AgentSession`, `ShellHost`, `MessagingHub`) belong in `feature/`.
5. **Fork glue stays at the edge** — RmlUi backend in `base/render/`; libp2p glue in `base/mesh/` (forks under `src/lib/`).

---

## Current state

The dependency hierarchy above is **enforced at the header level** for the former cycle points. Shared types live in their owning modules:

| Type | Header |
|------|--------|
| `LlmConfig` | `data/LlmConfig.h` |
| `ContextBudget` | `data/ContextBudget.h` |
| `TranscriptChatAction` | `messaging/ChatActionTypes.h` |
| `ConversationSummary` | `messaging/ThreadMemoryTypes.h` |
| `RelayEnvelope` (+ wire records) | `messaging/RelayEnvelope.h` |
| `ThreadChannel` | `messaging/ThreadChannel.h` |

**Intentional one-way edges** (not cycles): `ai/` → `messaging/` for thread context and compaction; `messaging/` → `crypto/` for E2E codecs; `crypto/` → `messaging/RelayEnvelope.h` for auto-key establishment.

**Platform / UI:** SDL pre-processing delegates theme sync and context-menu handling via `platform/AppEventHooks.h`, wired from `app/Application.cpp`.

Regressions are caught by [`scripts/check_base_includes.sh`](../../scripts/check_base_includes.sh).

**Where active development lives**

| Area | Base role | Feature / project pointer |
|------|-----------|---------------------------|
| Chat storage | SQLite thread store, codecs | [`projects/chat-storage-and-memory/`](../../projects/chat-storage-and-memory/) |
| E2E crypto | Message cipher, hybrid KEM, PSK | [`docs/contracts/MESSAGE_ENCRYPTION.md`](../../docs/contracts/MESSAGE_ENCRYPTION.md) |
| At-rest vault | PIN vault, profile secrets | [`projects/at-rest-crypto/`](../../projects/at-rest-crypto/) |
| Agent turns | LlmClient, TurnPlan, conversation | `feature/ai/` (`AgentSession`, turn pipeline) |
| Window shell | Theme, view catalog, working-set types | `feature/ui/ShellHost` |

---

## Adding or changing code

1. Find the module that **owns** the data, protocol, or external integration.
2. Follow the dependency principles above; if two modules need the same struct, split or move types before adding another cross-include.
3. Add tests in `src/base/<module>/tests/` (`*_test.cpp` files). One executable per folder is created automatically (`pp_browser_<module>_test`).
4. Run [`scripts/check_base_includes.sh`](../../scripts/check_base_includes.sh) before pushing.
5. Document externally visible behavior in [`docs/contracts/`](../../docs/contracts/) when wire formats, encryption, or on-disk layout change.

---

## Further reading

| Doc | Why |
|-----|-----|
| [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) | Five-layer layout (common/lib/base/feature/app), test placement |
| [`docs/architecture/ARCHITECTURE.md`](../../docs/architecture/ARCHITECTURE.md) | System overview (SDL, RmlUi, agent, shell) |
| [`docs/contracts/DATA_LAYOUT.md`](../../docs/contracts/DATA_LAYOUT.md) | On-disk paths and profile layout |
| [`docs/contracts/WIRE_SCHEMAS.md`](../../docs/contracts/WIRE_SCHEMAS.md) | Messaging wire formats |
| [`AGENTS.md`](../../AGENTS.md) | Agent-oriented map of the whole repo |
