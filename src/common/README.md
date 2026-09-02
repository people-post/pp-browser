# `src/common`

pp-browser **shared language**: tiny helpers and **contracts** (ports / vocabulary DTOs). Namespace / CMake: `pp_pbr_common`.

```
app → feature → domain → foundation → common → pp_common
```

**Rule:** common may depend only on FetchContent [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common) (`pp_common`) and other `common/` headers. It must **never** `#include` `base/`, `foundation/`, `domain/`, `feature/`, or `app/`.

Repo-wide North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).

Use **subdir paths** (`common/thread/…`, `common/chat/…`). Do not add top-level forwarding headers.

---

## Layout

| Subdir | Contents |
|--------|----------|
| *(root)* | Small utilities: `ValueJson`, `PbrCompat`, `SettledWait`, `StartupTiming`, `LengthPrefixedCodec`, `ByteRateLimiter`, `EmojiKey`, `CodedFailure` |
| [`directory/`](directory/) | Phone-book vocabulary: `DirectoryTypes`, `DirectoryJson`, `IDirectoryClient`, `RelayScope` |
| [`thread/`](thread/) | Thread/message records, history/blob DTOs, sync/memory, `ContextBudget`, role ports + `IThreadStore` |
| [`chat/`](chat/) | Chat payload/action DTOs, relay envelope, messaging limits, people-discovery blocks |
| [`media/`](media/) | `CallMediaHealth` |
| [`ui/`](ui/) | `WorkingSetTypes` |

### Thread role ports

Prefer the narrow port at call sites:

| Port | Role |
|------|------|
| `IThreadCatalog` | list/get/upsert/delete/find-or-create |
| `IThreadTranscript` | pages, append/update, context window, clear |
| `IThreadMemory` | conversation summary get/set/clear |
| `IThreadSync` | seq/epoch/sync_state/outbox |
| `IThreadStore` | inherits all four + `Flush()` (concrete stores implement this) |

Thin type headers (prefer over the umbrella): `ThreadRecordTypes`, `ChatHistoryTypes`, `ChatBlobTypes`, `ThreadChannel`. `ThreadTypes.h` is an umbrella include only.

---

## What belongs here

| Put it in common when… | Put it in foundation / domain when… |
|------------------------|------------------------------------|
| Two+ peers need the same **name** (id, enum, narrow DTO, port) without owning lifecycle | Something **owns** durable state, I/O, or a non-trivial algorithm |
| It is a header-light helper with no product ownership | It talks to SQLite, curl, SDL, RmlUi, Amp, filesystem vaults |
| It is a pure seam (`I*` + POD) for feature to wire | It implements that seam |

Litmus: *“Could two domain peers compile against this without linking each other?”* Yes → common.

---

## Independence

All units in this folder are peers: no cycles. Prefer new files over growing umbrellas.

---

## Adding code

1. Confirm it is not orchestration (that belongs in `feature/`).
2. Confirm no `base/` / foundation / domain includes.
3. Put new contracts in the matching subdir; update call sites to the subdir path (no forwarding headers).
4. Document wire/disk-facing shapes in [`docs/contracts/`](../../docs/contracts/) when they escape the process.
