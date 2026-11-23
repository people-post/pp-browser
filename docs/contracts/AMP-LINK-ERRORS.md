# AMP link manager error codes

Stable **`PeerLinkManager::Err`** values (stored in `CodedFailure<Err>::code`), as of pp-cpp-amp v6.1.0.  
`message` is developer detail only — not normative.

**Escalation rules** (hierarchical wrap, adapter vs owner, Phase 2+ tasks):
[CODED_FAILURE.md](CODED_FAILURE.md).

**Mesh port:** `IChatPeerLinks::Err` (pp-cpp-mesh `mesh/host/MeshPorts.h`) has the same codes — same names,
same numeric values. `AmpChatPeerLinks` maps `PeerLinkManager::Failure` → `IChatPeerLinks::Failure` by name
(`ToMeshLinkErr`, `mesh/host/AmpLinkErr.h`), not by casting the number; pp-cpp-mesh asserts every value against
Amp's and fails its build on an Amp code it does not map (since v10.1.0 — before it, the port lacked
`VersionMismatch`, and Amp's `VersionMismatch` read as the port's `Generic`). Feature/base code
should inspect **`IChatPeerLinks::Failure::GetCode()`** (or the `Is*` helpers) when using
`MeshHost::ChatDeps()` — not raw `PeerLinkManager` from feature headers.

| Code | Name | Meaning |
|------|------|---------|
| 0 | `Ok` | Success (empty `LinkRoe` / `ChannelRoe`) |
| 1 | `EndpointNotRegistered` | Dial alias has no registered multiaddr |
| 2 | `DialInBackoff` | Prior dial failed; backoff window active |
| 3 | `TooManyConcurrentDials` | `max_concurrent_dials` exceeded |
| 4 | `MaxLinksReached` | `max_links` table full |
| 5 | `AssociationNotReady` | Link not connected (channel open, etc.) |
| 6 | `LinkNotFound` | Alias not in link table after assoc |
| 7 | `NestedCarrierIncomplete` | Nested carrier establish missing inputs |
| 8 | `DialTimeout` | Handshake exceeded `dial_timeout` |
| 9 | `HandshakeFailed` | MSH / session setup failed (ours, or the responder's) |
| 10 | `TransportFailed` | ADP send/open failed, or the peer closed the association (table full, admission, shutdown) |
| 11 | `DualDialLost` | Dual-dial election lost |
| 12 | `ChannelOpenFailed` | L3 OPEN rejected or mux error |
| 13 | `VersionMismatch` | The peer speaks another wire or MSH version (version negotiation, hello or Close): it answered, and it or this side must update — `message` says which. Not a network failure: not retried as one |
| 14 | `Generic` | Unclassified (wrap fallback) |

A change to this table is a change in both repos that implement it: pp-cpp-amp (`PeerLinkManager::Err`) and
pp-cpp-mesh (`IChatPeerLinks::Err`, its `docs/ERRORS.md`).

In pp-browser, `VersionMismatch` reaches the user as the chat send notice `chat.notice.send.version_mismatch`
(`DirectSendErr::VersionMismatch` → `SendFailure::VersionMismatch`, when the relay cannot carry the message), and
a call's reach as `CallMediaErr::Unreachable` with `err=version_mismatch` on its diag line.

Parent layers should inspect **`PeerLinkManager::Failure::GetCode()`** (or **`IChatPeerLinks::Failure::GetCode()`** on the mesh port) only — not child-layer codes. When crossing an **owning** L4 boundary, wrap via `WrapLinkFailure` per [CODED_FAILURE.md](CODED_FAILURE.md).
