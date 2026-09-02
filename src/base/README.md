# `src/base`

**Transitional home** for what the North Star splits into **foundation** (shared kernel) and **domain** (independent peer libs). Physical folders still live here; target trees are `src/foundation/` and `src/domain/`.

```
app → feature → domain → foundation → common → pp_common
                 ↑ today’s code paths still use base/…
```

**Rule:** dependencies flow downward only. Code here may use `lib/` and `common/`; it must not `#include` from `feature/` or `app/`. Full rules: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).

Each top-level folder (and `ai/conversation`, `ai/mcp`) builds as its own static library — **`pp_base_<module>`**. The aggregate **`pp_base`** (`INTERFACE`; alias `pp_identity`) links all module libraries for feature and app code. See [`CMakeLists.txt`](CMakeLists.txt).

---

## Tier map (North Star)

| Tier | Inside-layer rule | Modules (today under `base/`) |
|------|-------------------|--------------------------------|
| **Foundation** | Ordered bands (not fully peer-independent) | `runtime`, `platform`, `error`, `i18n`, `data`, `crypto` (+ thin `mesh/identity` when treated as kernel) |
| **Domain** | **Strict independence** — no peer→peer includes/links | `people`, `messaging`, `net`, `mesh` (host/L4), `media`, `ai`, `ui`, `render` |

Cross-domain need → contract in [`src/common/`](../common/); wire in `feature/` (lifetimes in `app/`).

### Foundation bands (downward only)

```
runtime, platform(_core)
  ↑
error, i18n
  ↑
data
  ↑
crypto
```

### Domain peers (no edges between these)

`people` · `messaging` · `net` · `mesh` · `media` · `ai` · `ui` · `render`

---

## What belongs here

| Put it in foundation when… | Put it in domain when… | Put it in feature when… |
|----------------------------|----------------------|-------------------------|
| Every peer may need this **implementation** (paths, config, crypto primitives, runtime) | It owns one product engine/store/client/codec | It coordinates several domain modules into a user flow |
| It is shared kernel, not messaging/HTTP policy | Heavy, single-purpose product coding | Screen, hub, session lifecycle |

If unsure: *“Must every domain peer be allowed to link this?”* → foundation. *“Is this one capability among peers?”* → domain. *“Does it bind several peers together?”* → feature.

---

## Module map

```
src/base/
├── runtime/      [foundation] AppRuntime, coordinator, lifecycle, branding/version
├── platform/     [foundation] OS adapters — paths, assets, credentials, notifications; SDL glue
├── data/         [foundation] Config, profiles, session, schema, atomic file writes
├── error/        [foundation] App error categories
├── i18n/         [foundation] Localization catalogs
├── crypto/       [foundation] E2E/at-rest crypto primitives, PIN vault, KEM helpers
│
├── people/       [domain] Identity, contacts
├── net/          [domain] HTTP client, relay / registration / directory clients
├── messaging/    [domain] Threads, SQLite + JSON stores, relay/group/E2E codecs
├── mesh/         [domain] Amp product glue — host, ports, reachability, L4
├── media/        [domain] Call capture/playback + HW H264
├── ai/           [domain] LLM client, turn plans, structured parsing, MCP
│   ├── conversation/
│   └── mcp/
├── ui/           [domain] Theme, view catalog, chat widget DTOs, input glue
└── render/       [domain] Product RmlUi host/overlays (pp_base_render)
```

Forks live under `src/lib/` (Amp) and pp-cpp-ui (RmlUi), not here.

Start points:

- Bootstrap & config → `data/BootstrapTypes.h`, `data/Config.h`
- Chat persistence → `messaging/IThreadStore.h`, `messaging/SqliteThreadStore.h` (port → `common` over time)
- Agent transcript → `ai/conversation/Conversation.h`
- Shell theming → `ui/Theme.h`, `ui/ViewCatalog.h`

Includes (today): `#include "base/data/Config.h"`.

---

## Dependency design (current → target)

**Current (legacy DAG inside `base/`):** many one-way edges still exist (`messaging`→`people`, `net`→`messaging`/`people`, `ai`→`messaging`, …). They are listed as `LEGACY_DOMAIN_EDGES` in [`scripts/check_base_includes.sh`](../../scripts/check_base_includes.sh) — **new** peer→peer edges fail CI. Peeled into `common/`: `RelayScope`, directory vocabulary (`DirectoryTypes`/`DirectoryJson`/`IDirectoryClient`), `WorkingSetTypes`, `CallMediaHealth`, `ChatActionTypes` / `ThreadMemoryTypes`, `MessagingLimits`, messaging vocabulary (`ThreadChannel`, `ChatPayloadTypes`, `RelayEnvelope`, `ThreadTypes`, `SyncStateTypes`, `IThreadStore`, `ContextBudget`, `PeopleDiscoveryBlocks`). Foundation peel: `CurlSsl` → `platform_core`.

**Target:**

- Foundation: bands above only.
- Domain: **zero** peer→peer `PUBLIC_LIBS` / includes; use `common` contracts.
- Feature: bind ports to concrete types (`MessagingHub`, agent session, etc.).

### Principles for new code

1. **Do not add new domain→domain edges.** Extract a port/DTO to `common` or move orchestration to `feature`.
2. **Shared structs go low** — owner module, or `common` if two domain peers need the name.
3. **Heavy includes in `.cpp`** — keep headers lean.
4. **Orchestration stays up** — `AgentSession`, `ShellHost`, `MessagingHub` in `feature/`.
5. **Fork glue at the edge** — RmlUi in `render/`; Amp product glue in `mesh/`.

### Known shared types (today still in owning modules)

| Type | Header |
|------|--------|
| `LlmConfig` | `data/LlmConfig.h` |
| Attachment / relay helpers used by net | `messaging/ChatBlobRequestUtil.h`, `AttachmentCache.h`, `MessagingJson.h`, … |

Candidates to promote into `common` when peeling peer edges: people/net helper ports, narrow view DTOs. Remaining messaging vocab lives in [`src/common/`](../common/) (`ThreadTypes`, `IThreadStore`, `RelayEnvelope`, …).

---

## Adding or changing code

1. Classify the change as **foundation**, **domain**, **common contract**, or **feature** wiring.
2. Prefer extending foundation bands or an existing domain module over a new cross-peer include.
3. Add tests under `src/base/<module>/tests/`.
4. Run [`scripts/check_base_includes.sh`](../../scripts/check_base_includes.sh) before pushing.
5. Document wire/disk behavior in [`docs/contracts/`](../../docs/contracts/) when formats change.

---

## Further reading

| Doc | Why |
|-----|-----|
| [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) | North Star layers, independence, migration order |
| [`src/common/README.md`](../common/README.md) | Helpers + contracts |
| [`src/feature/README.md`](../feature/README.md) | Orchestration / wiring |
| [`docs/architecture/ARCHITECTURE.md`](../../docs/architecture/ARCHITECTURE.md) | System overview |
| [`AGENTS.md`](../../AGENTS.md) | Agent-oriented map |
