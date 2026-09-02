# `src/common`

pp-browser **shared language**: tiny helpers and **contracts** (ports / vocabulary DTOs). Namespace / CMake: `pp_pbr_common`.

```
app → feature → domain → foundation → common → pp_common
```

**Rule:** common may depend only on FetchContent [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common) (`pp_common`) and other `common/` headers. It must **never** `#include` `base/`, `foundation/`, `domain/`, `feature/`, or `app/`.

Repo-wide North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).

---

## What belongs here

| Put it in common when… | Put it in foundation / domain when… |
|------------------------|------------------------------------|
| Two+ peers need the same **name** (id, enum, narrow DTO, port) without owning lifecycle | Something **owns** durable state, I/O, or a non-trivial algorithm |
| It is a header-light helper with no product ownership | It talks to SQLite, curl, SDL, RmlUi, Amp, filesystem vaults |
| It is a pure seam (`I*` + POD) for feature to wire | It implements that seam |

Litmus: *“Could two domain peers compile against this without linking each other?”* Yes → common. *“Does this open sockets / encrypt blobs / run a state machine?”* → not common.

---

## Buckets

### A. Basics (utilities)

Cross-module helpers with no domain ownership. Keep these small.

| Header / unit | Role |
|---------------|------|
| `ValueJson.h` | Bridge to `pp::Value` / JSON |
| `PbrCompat.h` | Small compatibility shims |
| `SettledWait.h` | Async/sync wait helpers |
| `StartupTiming.h` | Startup timing probes |
| `LengthPrefixedCodec.*` | Length-prefixed wire framing |
| `ByteRateLimiter.h` | Simple rate limiting |
| `EmojiKey.h` | Emoji key helper |
| `CodedFailure.h` | `CodedFailure` template — escalation rules in [CODED_FAILURE.md](../../docs/contracts/CODED_FAILURE.md) |
| `RelayScope.h` | Relay scope bands / admission helpers (shared by mesh L4 + people hop policy) |
| `DirectoryTypes.h` / `DirectoryJson.*` | Directory / phone-book vocabulary + JSON (`DirectoryHit`, `MeshNodeHit`, `ContactId`, …) |
| `WorkingSetTypes.h` | Working-set candidate DTOs (ai + chat UI) |

### B. Domain contracts (North Star growth area)

pp-browser vocabulary and seams shared across **domain peers** (and used by feature wiring):

| Category | Intend to live here | Do not put here |
|----------|---------------------|-----------------|
| Identity vocabulary | peer/account id aliases, hop-policy *enums*, `RelayScope` | `IdentityStore`, contact DB |
| Messaging vocabulary | thread/message/channel ids, narrow store/view ports | `SqliteThreadStore`, full ChatPayload codec |
| Crypto seams | multi-peer ports (`IDekConsumer`-shaped, etc.) | vault / AEAD implementations |
| Net seams | blob/relay/directory *interfaces* | HTTP client implementations |
| Compat / versions | shared schema tokens needed by multiple peers | UI strings, Rml |

Prefer **pure headers**. Feature (or app) binds interfaces to concrete domain types.

---

## Independence

All units in this folder are peers: no cycles, no “module A owns module B.” Add new files; do not grow a god header.

---

## Adding code

1. Confirm it is not orchestration (that belongs in `feature/`).
2. Confirm no `base/` / foundation / domain includes.
3. If introducing a port, keep the owning **implementation** in foundation or domain.
4. Document wire/disk-facing shapes in [`docs/contracts/`](../../docs/contracts/) when they escape the process.
