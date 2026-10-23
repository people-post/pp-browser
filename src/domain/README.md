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
| `registration/` | `pp_domain_registration` | Registration finish / renew / nickname over the relay client and the identity store (headless) | conversations, pp-node |
| `node/` | `pp_domain_node` (+ `pp_domain_node_ledger`) | Node-role mesh services: hosting from capabilities, DHT / directory config, self row, org-seed admission; the ledger role (headless) | conversations, pp-node |
| `call_media/` | `pp_domain_call_media` | pp-browser's realtime protocol on the mesh: call-media transport, leg coordinator, frame crypto (headless) | calls, product, pp-call-probe |
| `peers/` | `pp_domain_peers` | Who a peer is: directory nicknames for strangers and the names threads / senders show (`DirectoryShadowCache`, `PeerDisplaySources`), signing / KEM keys from the relay directory, profile icons; the people → messaging bridges (`LocalKeysFromIdentity`, `GroupInviteStanding`) | conversations, gui |
| `attachments/` | `pp_domain_attachments` | Chat attachments: the peer-direct blob request and its mesh transport (`AmpChatBlobTransport`), CDN upload / fetch, background fetch + decrypt (`AttachmentFetchWorkflow`), the content-store binding (`AttachmentContent`) | conversations, gui |
| `delivery/` | `pp_domain_delivery` | Getting a chat envelope to a peer and its history back: where each peer is reached on the relay (`PeerRouteBook`: contacts, routes learned from verified envelopes, the directory), the mesh transports for direct chat (`AmpDirectChatTransport`) and chat history (`AmpChatHistoryTransport`); the outbox and retries and the control lane join it | conversations, broadcast, calls (through the hub) |

Tests sit in each module's `tests/` (`pp_browser_domain_<module>_test`).
