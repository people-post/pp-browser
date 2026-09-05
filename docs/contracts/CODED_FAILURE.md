# Coded failure escalation (per-module `ResultOrError`)

**Tier:** contract (in-process API; not on the wire)

Normative rules for **`CodedFailure<Err>` / `CodedRoe<T, Err>`** in pp-browser and pp-cpp-amp.
Wire formats and HTTP surfaces use their own version axes — this doc governs **how errors
escalate across owning layers** inside the process.

**Related:** object ownership [OWNERSHIP.md](../architecture/OWNERSHIP.md), mesh stack
[projects/adp/STACK.md](../../projects/adp/STACK.md), link code table
[AMP-LINK-ERRORS.md](AMP-LINK-ERRORS.md), UI catalog errors `AppError` in
`src/foundation/error/AppError.h`.

---

## Two typed error systems (do not merge)

| System | When to use |
|--------|-------------|
| **`CodedFailure<Module::Err>`** | Control flow inside a **domain layer** (mesh link, L4 coordinator, chat transport). Stable module-local `Err` ints. |
| **`AppError` (`Error` + category × code)** | User-facing copy and settings/auth/network UX (`Display`, `Log`). |

String-only `Error("…")` / `pp::Roe<T>` remains acceptable for codecs, one-off parsers, and
paths that never branch on failure class.

---

## Canonical module shape

Each **owning** module defines:

```cpp
enum class Err : int32_t { Ok = 0, /* module-specific */, Generic };

using Failure = CodedFailure<Err>;
using Roe = CodedRoe<void, Err>;           // or CodedRoe<T, Err>

// Parent wraps immediate child only:
static Failure WrapChildFailure(const Child::Failure& child) {
  switch (child.GetCode()) { /* map to parent Err */ }
  return Failure::Of(parent_err,
      detail::AppendFrom("parent: short detail", "child_label", child.message));
}
```

Helpers live in `src/common/CodedFailure.h` (`pbr::`) and per-layer copies in pp-cpp-amp
(`pp::amp::`, `pp::adp::`). New pp-browser modules use **`pbr::CodedFailure`**.

---

## Hierarchical ownership

Errors follow the same tree as object ownership ([A027](../../projects/adp/DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy)):

```text
ADP Connection
  └─ PeerLink
       └─ PeerLinkManager
            └─ IChatPeerLinks (adapter port — same codes as manager)
                 └─ L4 coordinators (circuit, DHT, call-media, media-relay, dial-back)
                      └─ feature Amp services (chat, history, blob)
                           └─ AppError / UI copy
```

**Rules:**

1. **Parent owns escalation** — only the parent may choose the **`Err` returned to its callers**.
2. **Inspect immediate child only** — `switch (child.GetCode())`; never branch on grandchild
   enums (e.g. L4 must not switch on `adp::Connection::Err`).
3. **Stable parent codes** — callers above the boundary use **`parent.GetCode()`** only
   ([AMP-LINK-ERRORS.md](AMP-LINK-ERRORS.md) for the link/port table).
4. **`message` is non-normative** — developer logs and support; not for wire or UI keys.

---

## Grandchildren and `AppendFrom`

Lower layers already append context when they wrap **their** child:

```text
amp link manager: transport failed [link: amp link: transport failed [adp: …]]
```

When an L4 coordinator wraps a link failure:

- Pass **`child.message`** into `AppendFrom(..., "link", child.message)`.
- Do **not** re-read ADP / PeerLink codes — the chain is in `message`.
- Add **one** new `[label: …]` segment for this layer only.

Grandchild detail is carried in the **message chain**, not in composed integer codes.

---

## Adapter vs owning boundary

| Boundary kind | Example | Wrap behaviour |
|---------------|---------|----------------|
| **Adapter port** (same semantics, feature isolation) | `AmpChatPeerLinks` : `PeerLinkManager` → `IChatPeerLinks` | **Identity map** — same `Err` values and `message`; no extra `AppendFrom`. |
| **Owning layer** (new module owns the operation) | `CircuitTunnelCoordinator` after `OpenChannel` fails | **`WrapLinkFailure`** — map to coordinator `Err` + `AppendFrom`. |

Feature code must not `#include "amp/link/*"` for errors; use `IChatPeerLinks::Failure` or
higher.

---

## Reference implementation (pp-cpp-amp)

| Child | Parent | Function |
|-------|--------|----------|
| `adp::Connection` | `PeerLink` | `PeerLink::WrapConnectionFailure` |
| `PeerLink` | `PeerLinkManager` | `PeerLinkManager::WrapPeerLinkFailure` |

pp-browser Phase 1 implements the **adapter** row:

| Child | Port | Function |
|-------|------|----------|
| `PeerLinkManager::Failure` | `IChatPeerLinks::Failure` | `ToLinkRoe` / `ToChannelRoe` in `MeshPorts.cpp` |

---

## Rollout status

### Phase 1 — Mesh port (done)

- [x] `IChatPeerLinks::LinkRoe` / `ChannelRoe` → `CodedRoe`
- [x] Identity map from `PeerLinkManager` preserving code + message
- [x] `IsAssociationNotReady`, `IsDialInBackoff`, `IsEndpointNotRegistered`
- [x] Tests: `mesh_ports_test.cpp`
- [x] [AMP-LINK-ERRORS.md](AMP-LINK-ERRORS.md) port alias note

### Phase 2 — L4 owning layers (in progress)

Each coordinator gets a **local `Err` enum**, `Failure` / `Roe` aliases, and
**`WrapLinkFailure(const …::Failure&)`** (name may vary). Wrap at every
failure exit that originated from association / channel open / nested
carrier — not string copy alone.

| Module | Primary files | Suggested `Err` themes | Status |
|--------|---------------|------------------------|--------|
| Dial-back | `reachability/AmpDialBackService.{h,cpp}` | `NotStarted`, `EndpointNotRegistered`, `LinkFailed`, `Timeout`, `ChannelFailed`, `ProtocolError`, `Generic` | **done** — wraps `PeerLinkManager::Failure` |
| DHT | `dht/AmpDhtService.{h,cpp}` | `NotStarted`, `LinkFailed`, `Timeout`, `ChannelFailed`, `NotFound`, `Generic`, … | **done** — wraps `PeerLinkManager::Failure`; `FindPeer` returns `FindPeerRoe` |
| Circuit tunnel | `l4/circuit/CircuitTunnelCoordinator.{h,cpp}` | `NotStarted`, `LinkFailed`, `Timeout`, `Rejected`, `Generic` | pending |
| Call-media leg | `l4/call_media/CallMediaLegCoordinator.{h,cpp}` | `LinkFailed`, `Timeout`, `Glare`, `Aborted`, `Generic` | pending |
| Media relay | `l4/media_relay/AmpMediaRelayCoordinator.{h,cpp}` | `LinkFailed`, `Timeout`, `QuoteRejected`, `Generic` | pending |

**Note:** Dial-back / DHT currently own `PeerLinkManager&` directly (mesh-internal), so
`WrapLinkFailure` takes `PeerLinkManager::Failure`. Feature-facing L4 that goes through
`IChatPeerLinks` wraps that port’s `Failure` instead — same numeric child table.

**Phase 2 checklist (per module):**

1. Add `enum class Err` + `using Failure = CodedFailure<Err>` (header or colocated `*Errors.h`).
2. Implement `WrapLinkFailure` with `switch` on the **immediate child's `Err` only**
   (`PeerLinkManager::Err` or `IChatPeerLinks::Err` — never ADP / PeerLink).
3. Replace `Error(link.error().message)` / opaque forwards with wrapped `Failure`.
4. Export `Is*` helpers only where multiple call sites branch the same way.
5. Unit/compose test: assert **`GetCode()`** at L4 boundary; keep message substring optional.
6. Document stable L4 codes in this file or a short module note when stabilized.

**Explicitly out of Phase 2:** codecs, SQLite, HTTP clients, `AppError` migration.

### Phase 3 — Feature Amp services

- [ ] `AmpDirectChatService`, `AmpChatHistoryService`, `AmpChatBlobService`, `AmpCircuitHopReach`:
      `WrapL4Failure` (or wrap link where L4 not yet coded).
- [ ] Retry/backoff branches on **`DialInBackoff`**, **`AssociationNotReady`**, **`EndpointNotRegistered`**
      via port/coordinator code — not message substring alone.
- [ ] Stop stripping codes at `Error(child.message)` unless converting to `AppError` with mapped category.

### Phase 4 — UI boundary

- [ ] Map terminal failures to **`AppError`** for display; codes drive logic, catalog drives copy.

### Phase 5 — pp-cpp-amp hardening (optional)

- [ ] `ChannelMux::Err` + wrap at `PeerLinkManager` where L3 rejection needs discrimination.
- [ ] Dedupe `CodedFailure.h` copies (amp L1/link vs `pbr::` vs future `pp_common`).

---

## Anti-patterns

- Forwarding **`child.error()`** into a **different** `CodedRoe` type without wrap (leaks foreign `Err`).
- **`Error(result.error().message)`** when the caller could use **`GetCode()`** on the same layer.
- Dual-maintaining **`IChatPeerLinks::Err`** and **`PeerLinkManager::Err`** tables with divergent values.
- Replacing **`AppError`** with per-module `CodedFailure` for PIN/auth/network UI strings.

---

## Tests

- Prefer **`EXPECT_EQ(failure.GetCode(), Module::Err::…)`** over message-only asserts.
- Message asserts remain useful for regression on **`AppendFrom`** chains.
- Adapter tests: port code **equals** manager code for the same operation (`mesh_ports_test.cpp`).
