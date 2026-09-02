# AMP link manager error codes

Stable **`PeerLinkManager::Err`** values (stored in `CodedFailure<Err>::code`).  
`message` is developer detail only — not normative.

**Mesh port:** `IChatPeerLinks::Err` uses the same numeric values. `AmpChatPeerLinks` maps
`PeerLinkManager::Failure` → `IChatPeerLinks::Failure` without renumbering. Feature/base code
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
| 9 | `HandshakeFailed` | MSH / session setup failed |
| 10 | `TransportFailed` | ADP send/open failed |
| 11 | `DualDialLost` | Dual-dial election lost |
| 12 | `ChannelOpenFailed` | L3 OPEN rejected or mux error |
| 13 | `Generic` | Unclassified (wrap fallback) |

Parent layers should inspect **`PeerLinkManager::Failure::GetCode()`** (or **`IChatPeerLinks::Failure::GetCode()`** on the mesh port) only — not child-layer codes.
