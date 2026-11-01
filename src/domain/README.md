# `src/domain`

**Shared capabilities** composed from [`src/lib/`](../lib/README.md) peers: what two or more features
(or pp-node, the probes) need, without any one feature's hub. Rules: [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md).

```
app → gui → feature → domain → lib → foundation → common
                      ↑ you are here
```

- **Belongs here:** a capability that combines two or more libs, or that two or more features use —
  and owns **no user-facing lifecycle** (no hub, session, screen). Anything with a hub / session /
  screen is a feature.
- **Downward only:** `lib/`, `foundation/`, `common/`, the mesh and Amp. Never `feature/`, `gui/`,
  `app/`; never RmlUi.
- **Ordered:** a module uses only the modules before it in [`CMakeLists.txt`](CMakeLists.txt);
  `scripts/check/check_domain_includes.sh` holds the same order.
- **Names what it links:** `pp_browser_add_domain_library` (`cmake/PpBrowserDomain.cmake`) — no
  `pp_base`, so headless modules stay linkable by pp-node (its allowlist:
  `cmake/PpBrowserNoUiDeps.cmake`).

| Module | CMake | What it does | Used by |
|--------|-------|--------------|---------|
| `registration/` (in [`capabilities/net/src/domain/registration/`](../../capabilities/net/src/domain/registration/), CR022) | `pp_domain_registration` | Registration finish / renew / nickname over the relay client and the identity store (headless) | conversations, pp-node |
| `node/` | `pp_domain_node` (+ `pp_domain_node_ledger`) | Node-role mesh services: hosting from capabilities, DHT / directory config, self row, org-seed admission; the ledger role (headless) | product (`ProductMesh`), pp-node |
| `call_media/` (in [`capabilities/calls/src/domain/call_media/`](../../capabilities/calls/src/domain/call_media/), CR022) | `pp_domain_call_media` | pp-browser's realtime protocol on the mesh: call-media transport, leg coordinator, frame crypto (headless) | calls, product, pp-call-probe |
| `peers/` (in [`capabilities/people/src/domain/peers/`](../../capabilities/people/src/domain/peers/), CR022) | `pp_domain_peers` | Who a peer is: directory nicknames for strangers (`DirectoryShadowCache`), signing / KEM keys from kept cards and the directory (`AccountKeyResolver`, on mesh's `IDirectoryClient`), the account-card policy, this account's own cards (`OwnCards`) | conversations, gui |
| `attachments/` (in [`capabilities/messaging/src/domain/attachments/`](../../capabilities/messaging/src/domain/attachments/), CR022) | `pp_domain_attachments` | Chat attachments: the peer-direct blob request and its mesh transport (`AmpChatBlobTransport`), CDN upload / fetch, background fetch + decrypt (`AttachmentFetchWorkflow`), the content-store binding (`AttachmentContent`) | conversations, gui |
| `delivery/` (in [`capabilities/delivery/src/domain/delivery/`](../../capabilities/delivery/src/domain/delivery/), CR022) | `pp_domain_delivery` | Getting an envelope to a peer, for messaging and realtime alike (CR020): where each peer is reached on the relay (`PeerRouteBook`: contacts, routes learned from verified envelopes, the directory) and on the mesh (`AmpDirectChatTransport`; the chat bundle `PeerTransports` — direct chat, history and blob transports on one Amp peer-links instance — is `feature/conversations`'), account cards over the mesh, the outbox (`DeliveryOutbox`: Amp or relay, retries, outcomes to the owner), and the control lane (`ControlLane`: identified system messages — call signaling — E2E under the pair's e2e_public session, `E2ePublicSessionKeys`) | conversations, broadcast, calls (through the hub) |

Tests sit in each module's `tests/` (`pp_browser_domain_<module>_test`).
