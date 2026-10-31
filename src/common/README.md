# `src/common`

pp-browser **shared language**: tiny helpers and **contracts** (ports / vocabulary DTOs). Namespace / CMake: `pp_pbr_common`.

```
app → feature → domain → lib → foundation → common → pp_common
```

**Rule:** common may depend only on FetchContent [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common) (`pp_common`), FetchContent [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto) (`pp_crypto`, for shared Base64 in wire JSON and the keyed hash of `ShareableLine::Ref`), and other `common/` headers. It must **never** `#include` `foundation/`, `lib/`, `feature/`, or `app/`.

Repo-wide North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).

Use **subdir paths** (`common/thread/…`, `common/chat/…`). Do not add top-level forwarding headers.

---

## Layout

| Subdir | Contents |
|--------|----------|
| *(root)* | Small utilities: `PbrCompat` (bridges pp-cpp-common's names — `ValueJson`, `SettledWait`, `CodedFailure`, `AtomicFileWrite`, `MetricsRegistry` — into `pbr`), `MeshAlias` (`mesh::` = pp-cpp-mesh's `pp::mesh`), `StartupTiming`, `LengthPrefixedCodec`, `ByteRateLimiter`, `EmojiKey`, `ArgsText`, `ShareableLine` + its channels `Metrics` / `Diag` (lines that may leave the device — docs/contracts/DIAGNOSTICS.md) |
| [`net/`](net/) | `HttpTransport` / `HttpResponse` injectable POST seam |
| [`directory/`](directory/) | Phone-book glue: `DirectoryJson`, `IdentityTypes`, trust / signing access ports. The rows themselves (`DirectoryTypes`, `IDirectoryClient`, `RelayScope`, `MeshHopTypes`, `MeshHopDial`) are pp-cpp-mesh's vocabulary, `mesh/types/` |
| [`thread/`](thread/) | Thread/message records, history/blob DTOs, sync/memory, `ContextBudget`, role ports + `IThreadStore` |
| [`chat/`](chat/) | Chat payload/action DTOs, relay envelope/stream key, `MessagingJson`, messaging limits, people-discovery blocks, `AttachmentDownloadPolicy` |
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

Litmus: *“Could two lib peers compile against this without linking each other?”* Yes → common.

**Testing:** prefer **unit** coverage for helpers and pure contract adapters. Doctrine: [TESTING.md](../../docs/architecture/TESTING.md).

---

## Independence

All units in this folder are peers: no cycles. Prefer new files over growing umbrellas.

---

## Adding code

1. Confirm it is not orchestration (that belongs in `feature/`).
2. Confirm no `base/` / foundation / domain includes.
3. Put new contracts in the matching subdir; update call sites to the subdir path (no forwarding headers).
4. Document wire/disk-facing shapes in [`docs/contracts/`](../../docs/contracts/) when they escape the process.
