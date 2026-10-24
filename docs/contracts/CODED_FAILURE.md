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

Helpers live in pp-cpp-common's `common/CodedFailure.h` (`pp::`, bridged into `pbr` by `PbrCompat`)
and per-layer copies in pp-cpp-amp (`pp::amp::`, `pp::adp::`). New pp-browser modules use
**`CodedFailure`** (`pp::CodedFailure`).

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
| **Owning layer** (new module owns the operation) | `CircuitClientCoordinator` after `OpenChannel` fails | **`WrapLinkFailure`** — map to coordinator `Err` + `AppendFrom`. |

Feature code must not `#include "amp/link/*"` for errors; use `IChatPeerLinks::Failure` or
higher.

---

## Reference implementation (pp-cpp-amp)

| Child | Parent | Function |
|-------|--------|----------|
| `adp::Connection` | `PeerLink` | `PeerLink::WrapConnectionFailure` |
| `PeerLink` | `PeerLinkManager` | `PeerLinkManager::WrapPeerLinkFailure` |

pp-browser implements the **adapter** row:

| Child | Port | Function |
|-------|------|----------|
| `PeerLinkManager::Failure` | `IChatPeerLinks::Failure` | `ToLinkRoe` / `ToChannelRoe` in `MeshPorts.cpp` |

---

## Where codes are used

| Layer | Module | `Err` |
|-------|--------|-------|
| Mesh port | `IChatPeerLinks::LinkRoe` / `ChannelRoe` (identity map from `PeerLinkManager`, code + message kept; `IsAssociationNotReady`, `IsDialInBackoff`, `IsEndpointNotRegistered`) | the link's |
| L4 (pp-cpp-mesh) | dial-back, DHT (`FindPeerRoe`), circuit tunnel, circuit reach, punch walk, peer reach, media relay | each module's own (`CircuitTunnelErr`, `CircuitReachErr`, `PunchIntroducerWalk::Err`, `PeerReachCoordinator::Err`, `MediaRelayErr`, …); a remote refusal is a wire `code` (pp-cpp-mesh `docs/ERRORS.md`) |
| Call media | `CallMediaLegCoordinator`, `ICallMediaTransport::on_failed`, `CallMediaConnectCoordinator` (`WrapReachFailure`, `CallMediaRoe`) | `CallMediaErr` (`StaleLink`, `StreamClosed`, …) |
| Chat send | `IDirectMessageClient` (`AmpDirectChatTransport::WrapLinkFailure`), `HttpRelayClient::Send` | `DirectSendErr`; relay `AppError` codes → `lib/messaging/DeliveryNotice` (`SendFailureFromDirect` / `SendFailureFromRelay`) |

Dial-back / DHT own `PeerLinkManager&` (mesh-internal), so they wrap `PeerLinkManager::Failure`;
feature-facing L4 that goes through `IChatPeerLinks` wraps that port's `Failure` — the same numeric
child table.

**Decisions read codes, never message text:** retry / backoff, "dial a fresh link next time"
(`CallMediaErr::StaleLink`), a clean stream close (`CallMediaErr::StreamClosed`), chat send notices.
Group-membership controls dispatch on their control type (`RelayReceivePipeline`), not text.

**Not coded:** codecs, SQLite, HTTP clients beyond the relay send, and peer transports whose callers
treat every failure the same (`AmpChatHistoryTransport`, `AmpChatBlobTransport`: their callers fall
back to the relay / CDN). Code one when a caller or the UI needs to tell its failures apart.

## Coding a layer

1. Add `enum class Err` + `using Failure = CodedFailure<Err>` (header or colocated `*Errors.h`).
2. Implement `WrapLinkFailure` (name may vary) with a `switch` on the **immediate child's `Err`
   only** (`PeerLinkManager::Err` or `IChatPeerLinks::Err` — never ADP / PeerLink).
3. Wrap at every failure exit that came from association / channel open / a nested carrier; never
   `Error(child.message)`.
4. Export `Is*` helpers only where several call sites branch the same way.
5. Test **`GetCode()`** at the boundary; a message substring is optional.
6. Terminal failures shown to the user map to `AppError`: codes drive logic, the catalog drives copy.

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
