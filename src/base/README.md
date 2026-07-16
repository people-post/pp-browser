# `src/base` — product primitives

**Tier:** architecture (module guide)

The **base** layer holds pp-browser-specific building blocks: one store, one client, one parser per concern. Feature code in [`src/feature/`](../feature/) composes these into workflows and screens; the app layer wires everything at startup.

Layer rule (repo-wide):

```
app → feature → base → common
```

Base must not `#include` from `feature/` or `app/`. See [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) for the full four-layer picture.

## CMake target

All modules compile into a single static library:

| Target | Alias | Notes |
|--------|-------|-------|
| `pp_base` | `pp_identity` | Links `pp_common`, RmlUi, SDL3, curl, SQLite, libsodium, libp2p |

Sources are listed in [`CMakeLists.txt`](CMakeLists.txt). libp2p host glue under [`src/libp2p/integration/host/`](../libp2p/integration/host/) is compiled into `pp_base` (fork sidecar, not a base subfolder).

Colocated unit tests live in `*/tests/` and register via `pp_browser_register_tests(...)` when `PP_BROWSER_BUILD_TESTS` is on.

## Directory map

```
src/base/
├── error/          App error taxonomy and display helpers
├── platform/       OS/SDL, paths, assets, threading, credentials, notifications
├── i18n/           Localization catalogs and locale detection
├── data/           Config, profiles, session, schema, atomic file I/O
├── crypto/         E2E message crypto, hybrid KEM, at-rest PIN vault
├── people/         Identity, contacts, Ed25519 signing
├── net/            HTTP client, relay/registration/directory service clients
├── messaging/      Thread types, JSON/SQLite stores, wire codecs, E2E ingest
├── ai/             LLM client, turn plans, conversation, MCP client/runtime
│   ├── conversation/
│   └── mcp/
└── ui/             Theme, view catalog, shell/working-set types, input glue
```

Includes use the repo root `${CMAKE_SOURCE_DIR}/src` with layer-prefixed paths:

```cpp
#include "base/data/Config.h"
#include "common/Error.h"
```

## Module roles

| Module | Responsibility | Key entry points |
|--------|----------------|------------------|
| **error** | Product error categories (`ErrorCategory`, `Err::*`) over `common/Error.h` | `AppError.h` |
| **platform** | SDL lifecycle, desktop/Android/iOS path & asset providers, `BrowserThread`, credential store, local notifications, background sync | `Platform.h`, `BrowserThread.h`, `AssetIO.h` |
| **i18n** | String catalogs loaded from JSON assets | `LocalizationService.h` |
| **data** | `AppConfig`, profile registry, session store, schema versioning, LLM presets | `Config.h`, `ProfileRegistry.h`, `BootstrapTypes.h` |
| **crypto** | Message encryption, hybrid KEM key establishment, PSK bundles, replay window, PIN-derived at-rest vault | `MessageCipher.h`, `AutoKeyEstablishment.h`, `DataKeyVault.h` |
| **people** | Local identity, contacts persistence, signing | `IdentityStore.h`, `ContactsStore.h` |
| **net** | HTTP transport and signed calls to relay, registration, directory services | `HttpClient.h`, `ServiceClientFactory.h` |
| **messaging** | Chat thread model, SQLite/JSON persistence, relay/group/E2E codecs, envelope signing, `@ai` parsing | `ThreadTypes.h`, `IThreadStore.h`, `SqliteThreadStore.h` |
| **ai** | LLM HTTP client, turn plan/trace, structured chat parsing, prompt building, context policies, MCP | `LlmClient.h`, `TurnPlan.h`, `conversation/Conversation.h`, `mcp/McpClient.h` |
| **ui** | RmlUi-facing theme/view catalog, working-set panel types, input coordinator, context menu host | `Theme.h`, `ViewCatalog.h`, `WorkingSetTypes.h` |

Contract docs for several areas: [`docs/contracts/`](../../docs/contracts/) (encryption, wire schemas, data layout, compatibility).

## Dependency design (target)

Within base we prefer **small, mostly independent modules** with **downward-only** `#include` edges. The intended hierarchy is not fully enforced yet (see [Current state](#current-state)), but new code should aim for this shape:

```
                    common
                       ↑
         ┌─────────────┼─────────────┐
         │             │             │
      error          i18n        (leaf wrappers)
         │             │
         └──────┬──────┘
                ↓
            platform          OS paths, SDL, threading
                ↓
              data            config / profiles / session (no ai types)
         ┌──────┴──────┐
         ↓             ↓
      crypto        people         keys, identity, contacts
         └──────┬──────┘
                ↓
               net              HTTP + service clients
                ↓
           messaging           threads, codecs, stores
          ┌─────┴─────┐
          ↓           ↓
         ai           ui           LLM pipeline; RmlUi shell types
```

Guidelines when adding or moving code:

1. **Prefer leaf modules** — `error` and thin type headers should not pull in heavy dependencies.
2. **Shared DTOs live low** — wire structs and config structs belong in the module that owns persistence or the protocol, not in a consumer.
3. **Header includes define coupling** — a header `#include` creates compile-time dependency; keep impl-only includes in `.cpp` when possible.
4. **No base → feature** — orchestration that needs multiple base modules belongs in `src/feature/`.
5. **Fork boundaries** — RmlUi integration types stay in `base/ui` or `src/render/integration/`; libp2p public API only via `src/libp2p/fork/include/`.

## Current state

All base sources build as **one** `pp_base` target today. Cross-module coupling is real and documented here so refactors can shrink it over time.

### Observed header-level dependencies

| Module | Depends on (other base modules, headers) |
|--------|------------------------------------------|
| error | — |
| i18n | — |
| platform | data |
| data | **ai** |
| crypto | **messaging** |
| people | crypto |
| net | data, messaging, people |
| messaging | ai, crypto, people |
| ai | data, messaging, ui |
| ui | — |

Implementation (`.cpp`) adds further edges — e.g. `platform` → messaging/ui for background sync and SDL event routing, `i18n` → platform for asset loading, `error` → i18n for localized messages.

### Known circular includes (header level)

These are the main places where the graph is not yet acyclic:

| Cycle | Cause | Likely fix direction |
|-------|-------|----------------------|
| **data ↔ ai** | `Config.h` includes `LlmClient.h` and `ConversationTypes.h`; MCP runtime includes `Config.h` | Extract `LlmConfig` / MCP config structs into `data/` (or a neutral types header) so `data` does not depend on `ai` |
| **crypto ↔ messaging** | `AutoKeyEstablishment.h` uses `RelayEnvelope` / `ChatTargetKey` from `ThreadTypes.h`; messaging E2E headers include crypto types | Move shared wire/target types to a small shared header (e.g. `messaging/wire/` or `crypto/wire/`) owned below both |
| **ai ↔ messaging** | `ThreadTypes.h` includes `ConversationTypes.h`; compaction/context policies bridge thread store and conversation | Split chat-thread DTOs from AI conversation DTOs; depend on shared neutral types where both need the same shape |

Other coupling worth knowing but not full header cycles:

- **platform → ui** — `SdlAppEvents.cpp` forwards events into `ContextMenuHost` / `Theme` (could move behind a feature-layer callback).
- **messaging hub** — largest cross-module consumer (crypto, people, net, ai types in thread model).

No upward `#include` of `feature/` or `app/` was found in base — the outer layer rule is respected.

### Maturity notes

| Area | State |
|------|-------|
| **data / platform** | Stable bootstrap path (`BootstrapTypes.h`, path providers, config JSON) |
| **messaging** | SQLite thread store and relay codecs in active use; JSON store retained for compatibility |
| **crypto** | E2E and at-rest vault implemented; see [`projects/e2e-message-crypto/`](../../projects/e2e-message-crypto/) and [`projects/at-rest-crypto/`](../../projects/at-rest-crypto/) |
| **ai** | Turn plan pipeline, conversation policies, MCP client; feature layer owns `AgentSession` / turn execution |
| **ui** | Theme and view catalog; full shell composition lives in `feature/ui/ShellHost` |
| **Dependency hygiene** | Aspirational hierarchy above; cycles listed above remain acceptable technical debt until typed splits land |

## Adding code here

**Litmus test** (from SRC_LAYOUT): base code is product-specific but **single-purpose** — one store, one client, one parser. If you are coordinating multiple modules into a user-visible workflow, put it in `feature/`.

Checklist:

1. Pick the module that **owns the data or protocol** you are implementing.
2. Add `#include "base/…"` only from modules **at or below** your target layer in the diagram above.
3. Put tests beside the module: `src/base/<module>/tests/`, register the directory in [`CMakeLists.txt`](CMakeLists.txt).
4. Avoid new header cycles — if two modules need the same struct, move the struct to the lower module or a dedicated types header.
5. Document wire/crypto behavior in [`docs/contracts/`](../../docs/contracts/) when behavior is externally visible.

## Related docs

| Doc | Topic |
|-----|-------|
| [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) | Four-layer layout and fork sidecars |
| [`docs/architecture/ARCHITECTURE.md`](../../docs/architecture/ARCHITECTURE.md) | End-to-end system view |
| [`docs/contracts/DATA_LAYOUT.md`](../../docs/contracts/DATA_LAYOUT.md) | On-disk layout |
| [`docs/contracts/WIRE_SCHEMAS.md`](../../docs/contracts/WIRE_SCHEMAS.md) | Messaging wire formats |
| [`docs/contracts/MESSAGE_ENCRYPTION.md`](../../docs/contracts/MESSAGE_ENCRYPTION.md) | E2E crypto |
| [`docs/ui/RML_PROFILE.md`](../../docs/ui/RML_PROFILE.md) / [`RCSS_PROFILE.md`](../../docs/ui/RCSS_PROFILE.md) | UI generation constraints (ai + ui) |
