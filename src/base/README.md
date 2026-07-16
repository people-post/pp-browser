# `src/base`

Product-specific **primitives** for pp-browser: stores, clients, codecs, and UI building blocks that feature code composes into screens and workflows.

```
app        wires startup, profiles, global services
feature    chat, shell, agent session, messaging hub
base       ← you are here
common     logger, ResultOrError, task runner (app-agnostic)
```

**Rule:** dependencies flow downward only. Base may use `common/`; it must not `#include` from `feature/` or `app/`. Repo-wide layout: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).

Everything under this tree builds as one static library, **`pp_base`** (alias `pp_identity`). Sources are in [`CMakeLists.txt`](CMakeLists.txt).

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

Ten top-level folders. Two sub-trees under `ai/`.

```
src/base/
├── platform/     OS & runtime — SDL, paths, assets, BrowserThread, credentials, notifications
├── data/         Config, profiles, session, schema version, atomic file writes
├── error/        App error categories on top of common/Error.h
├── i18n/         Localization catalogs (JSON assets)
│
├── people/       Identity, contacts, Ed25519 signing
├── crypto/       E2E message crypto, hybrid KEM, at-rest PIN vault
├── net/          HTTP client, relay / registration / directory clients
├── messaging/    Threads, SQLite + JSON stores, relay/group/E2E codecs
│
├── ai/           LLM client, turn plans, structured chat parsing, MCP
│   ├── conversation/   transcript, context policies, compaction
│   └── mcp/            MCP client, runtime, schema adapter
└── ui/           Theme, view catalog, working-set types, input glue (RmlUi-facing)
```

**Domain grouping (mental model):**

| Domain | Modules | Typical question it answers |
|--------|---------|----------------------------|
| **Runtime** | platform, data, error, i18n | Where do files live? What is configured? How do we report errors? |
| **Identity & trust** | people, crypto | Who am I? How are messages and disk encrypted? |
| **Connectivity** | net, messaging | How do we reach services and peers? How are threads stored and encoded? |
| **Intelligence & presentation** | ai, ui | How do we call the LLM and shape context? What RmlUi assets/types does the shell need? |

Start points when exploring:

- Bootstrap & config → `data/BootstrapTypes.h`, `data/Config.h`
- Chat persistence → `messaging/IThreadStore.h`, `messaging/SqliteThreadStore.h`
- Agent transcript → `ai/conversation/Conversation.h`
- Shell theming → `ui/Theme.h`, `ui/ViewCatalog.h`

Includes use the repo root: `#include "base/data/Config.h"`.

---

## Dependency design

**Goal:** small modules that are as independent as possible, arranged in a **hierarchy without cycles**. Shared types live in the module that owns the data or protocol, not in a consumer.

Intended direction (not fully realized yet — see below):

```
common
  ↑
platform, error, i18n          thin / leaf where possible
  ↑
data                           config & persistence (should not depend on ai)
  ↑
crypto, people
  ↑
net
  ↑
messaging
  ↑
ai, ui                         parallel consumers of messaging + data
```

**Principles for new code:**

1. **Downward includes only** — when module A needs a type from B, B should not include A’s headers.
2. **Shared structs go low** — if two modules need the same DTO, move it to the lower owner (or a dedicated `*Types.h` in that owner).
3. **Headers are contracts** — prefer heavy includes in `.cpp` files; keep headers lean to limit compile-time coupling.
4. **Orchestration stays up** — multi-module workflows (`AgentSession`, `ShellHost`, `MessagingHub`) belong in `feature/`.
5. **Fork glue stays at the edge** — RmlUi in `base/ui` + `src/render/integration/`; libp2p public API via `src/libp2p/fork/include/` (host bootstrap is compiled into `pp_base`).

---

## Current state (honest snapshot)

The layout above reflects **intent**. The codebase is a single `pp_base` target with pragmatic cross-includes that we are gradually straightening out.

**What works well today**

- Outer layer discipline is respected — no base → feature/app includes.
- Clear module homes for most concerns (crypto, people, net, ai sub-trees).
- Colocated tests under `*/tests/`, registered from [`CMakeLists.txt`](CMakeLists.txt).

**Known coupling (technical debt, not blockers)**

- **data ↔ ai** — `Config.h` embeds LLM/conversation config types defined in `ai/`.
- **crypto ↔ messaging** — key-establishment APIs reference relay/thread wire types from `messaging/`.
- **ai ↔ messaging** — thread model and conversation transcript share message-shape types.
- **platform → ui** — SDL event path touches context-menu/theme helpers (impl-only today).

These cycles are understood; fixes generally mean extracting neutral type headers into the owning lower module. New work should **not add** cycles.

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
3. Add tests in `src/base/<module>/tests/` and register the directory in [`CMakeLists.txt`](CMakeLists.txt) if new.
4. Document externally visible behavior in [`docs/contracts/`](../../docs/contracts/) when wire formats, encryption, or on-disk layout change.

---

## Further reading

| Doc | Why |
|-----|-----|
| [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) | Full four-layer layout, fork sidecars, test placement |
| [`docs/architecture/ARCHITECTURE.md`](../../docs/architecture/ARCHITECTURE.md) | System overview (SDL, RmlUi, agent, shell) |
| [`docs/contracts/DATA_LAYOUT.md`](../../docs/contracts/DATA_LAYOUT.md) | On-disk paths and profile layout |
| [`docs/contracts/WIRE_SCHEMAS.md`](../../docs/contracts/WIRE_SCHEMAS.md) | Messaging wire formats |
| [`AGENTS.md`](../../AGENTS.md) | Agent-oriented map of the whole repo |
