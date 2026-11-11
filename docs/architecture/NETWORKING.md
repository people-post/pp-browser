# Networking doctrine

**Tier:** architecture  
**Status:** Product north star (2026-08-31 — D10 Amp hard-require)

pp-browser uses **HTTP** plus a **peer mesh**. Product peer mesh is **AMP** (UDP + Session/Channel) under [`projects/adp`](../../projects/adp/); vendored libp2p shrinks toward PeerId/crypto helpers ([A017](../../projects/adp/DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)). Agents must not introduce a third realtime stack (e.g. WebRTC/libdatachannel) as the product path.

## Planes

| Plane | Preferred role |
|-------|----------------|
| **HTTP** | Contact **org backends** when reachable: Brief APIs, relay store/history, client-compat, quotes/billing UX, settlement orchestration |
| **Peer mesh (AMP)** | **Peer data exchange** when `mesh.mesh_enabled` (default true): messaging, call signaling, **call media**, mesh capabilities (`media_relay`, circuit, …). Amp bind failure **fails mesh start** (D10 — no TCP underlay fallback). `mesh_enabled=false` leaves peer mesh off |

**Relay request hashing:** relay API requests sit behind CloudFront origin access control, which requires `x-amz-content-sha256` (hex SHA-256 of the exact body, empty body included) on every POST/PUT. `HttpClient::Post`/`Put`, `LlmClient` and `SseClient` add it; presigned storage URLs (`HttpClient::Put` with `add_content_sha256=false`, `PutFile*`) do not.

## Settlement (pricing)

| Path | When |
|------|------|
| **HTTP backend** | **Preferred** when available — receipts, rate cards, user-facing bills |
| **Direct chain settle** | **Backup** when HTTP is not an option (or policy prefers trust-minimized pay) |

Meters and hop **quotes** still originate on the mesh ([p2p-mesh](../../projects/p2p-mesh/) N019–N020). Backend/chain are how those obligations **settle**, not a second media path.

## Amp mesh (D10)

**AMP** ([`projects/adp`](../../projects/adp/)) is the only product underlay when `mesh_enabled` is on: UDP + MSH + channels; ch0 replaces Identify; Amp UDP accept is always on. MeshHost does **not** start TCP listen, Identify, DialBack, or libp2p circuit/media-relay hosting.

**LAN keep:** mDNS TXT `amp_udp=`, PreferLocal / invite Amp multiaddrs (`BuildAmpLanAdvertisedAddrs`), contact/ch0 ADP addrs. **WAN inbound chrome:** Amp dial-back (D8) + optional UPnP UDP; needs ADP bootstrap peers for seed dial. Global `/ip6` advertise + probe targets preferred over private `/ip4` (`RankAmpDialMultiaddrs`, N013); Reachable-via-v6 when `has_global_ipv6` and dial-back succeed.

The **vendored** fork under [`src/lib/libp2p/`](../../src/lib/libp2p/) is still in-tree, but product binaries link **PeerId/crypto helpers only** (`p2p_peer_id`, `p2p_wire` — [A017](../../projects/adp/DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)). Idle Host/stream sources are unlinked pending delete. Mesh work continues to deepen:

- Peer discovery and dialability (ch0 / mDNS `amp_udp`, contacts, bootstrap)
- Reachability (Amp UDP accept, circuit nested Session, SoftMigrate, Amp dial-back)
- Routing and transmission (L3 channels, FRAG/QoS, budgets)
- Price incentives (quotes, ceilings, volunteer → paid regulation)

**Network changes** (Wi-Fi ↔ cellular, new address, back online): the platform `NetworkMonitor` feeds the mesh, which re-validates every direct link within 2 s and re-learns its addresses; calls re-anchor — [MESH.md § Local network change](MESH.md#local-network-change-call-path-resilience-k5).

**Hop reachability** continues in-mesh ([media-hop-reachability](../../projects/media-hop-reachability/)), including planned **Amp Coordinated Punch** ([HOLE_PUNCH.md](../../projects/media-hop-reachability/HOLE_PUNCH.md)). SoftMigrate must not grow a parallel NAT toolkit.

**Ownership planes:** Profile (app/node-local secrets + identity) → **MeshHost** (Amp composition root) → **ConversationsHub** / ConversationsCore, with **CallStack** and **BroadcastHub** riding it as participants (app-only, composed by `ProductStack`) → **ConversationsFacade** / CallUiBackend (UI).

See [p2p-mesh](../../projects/p2p-mesh/) (N022+) and [adp](../../projects/adp/).

**L4 protocol kinds:** freeze growth around seven conversation shapes (identify / discover / reach / circuit / rpc / blob / realtime) — [L4_PROTOCOL_KINDS.md](../contracts/L4_PROTOCOL_KINDS.md) ([A028](../../projects/adp/DECISIONS.md#a028--l4-protocol-kinds--seven-conversation-shapes), [N030](../../projects/p2p-mesh/DECISIONS.md#n030--adopt-l4-protocol-kinds-gate)). Do not add a new `protocol_id` per feature.

## Address disclosure (privacy)

Who may learn this device's IP address is one setting, `mesh.direct_connections` — `everyone` | `contacts` (default) | `friendly` | `nobody`; Blocked contacts never; the Node role is always `everyone` ([projects/privacy](../../projects/privacy/) T1, P001–P004). The hub publishes an `AddressDisclosureGate` (pp-cpp-mesh `mesh/types/AddressDisclosure.h`) from the setting and the address book, republished on every contact or setting change; mesh and call paths read it on any thread. For a peer outside the audience:

| Path | Behaviour | Code |
|------|-----------|------|
| Punch target | Declines the introducer's `offer` (no candidates, no burst) | `PunchServer` (`pp_punch_served_total{role=target_declined}`) |
| Calls | Relay only: no direct dial, no punch, no move onto a direct link; the relay anchors | `CallStack::PathPolicyFor` → `RelayOnlyPolicy` |
| Call signalling | Invite / accept carry no listen addresses; a signalling punch is not answered | `CallSessionWorkflow`, `SignalingPunchExchange` |
| Chat, attachments, history | No new dial — an existing link, else the Brief relay | `DisclosureGatedPeerLinks` in `MeshDeliveryOrchestrator` |

A link that is already up may carry traffic (P003). Relays and seeds use the plain links (relay operators see addresses: privacy T4); so do peer-announce and broadcast viewing — watching a publisher is the viewer's choice (P005). The ch0 capability carries our listen multiaddrs only to allowed peers (`MeshHost` → Amp `SetListenAddrDisclosure`). Registration publishes our listen addresses to the org directory — readable by anyone — only when the audience in force is `everyone` (a Node, or a user who chose it; `PublishesAddresses`); otherwise it registers the PeerId alone and peers reach us by it through relays. Addresses registered before the change stay on the server until the next registration.

**Inbound control** (privacy T3): a Blocked contact's direct messages and call control are discarded on receipt (`RelayReceivePipeline::SetContactTrust`) and its Amp link is dropped on connect (`MeshHost`); `call_invite_policy` (Me → Security → Who can call me) drops invites from outside `contacts_only` | `nobody` silently (`everyone` is the default) (`CallSessionManager::AllowsInboundCall`).

**Who is a relay** (#379): circuit rendezvous — parking and circuit dialing — goes only through circuit relays: the configured org seeds and peers the directory or the DHT lists with `circuit_relay` (`KeepCircuitRelays` in `MeshHopPolicy`, applied in `MeshConnectivityWiring`). A contact's phone or a node the DHT merely knows is no relay; contacts stay punch introducers. A relay that refuses a circuit is tried after the others for ten minutes (pp-cpp-mesh `CircuitRelayRefusals`). Directory listings and a node's own published addresses leave out private-range and local-scope addresses beside a public one (`DialWorthyMultiaddrs`): a relay's container address is no use to a reader and only made dials fall back to it.

**Relay choice against dishonest nodes** ([N032](../../projects/p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes)): a node's own claims (its directory row) only ever demote it, and nothing is dropped for distrust, only tried later (`RelaySetRules` in `lib/people`).
- **Bands:** the park / circuit surface (`MeshConnectivityWiring`) and the group media hop ranking (`CallHopRanking`) order their relays in bands, order kept within each:
  - the rest;
  - a second non-anchor relay of an operator (account id) already listed;
  - a node said to run older than `mesh.preferred_min_node_version` (or saying nothing);
  - a node, or any node of an operator, this profile demoted.
- **Anchors:** on the park / circuit surface an anchor (org seed or Friendly contact's node, as for trusted relays) leads, and at most two others come between anchors while anchors remain, so unknown nodes cannot crowd them out.
- **Audit:** each policy refresh checks connected directory nodes' claims against what they told over channel 0, and a dial that met another wire version. A claim contradicted in the liar's favour (our wire version, a newer version than it runs, a feature it lacks) demotes the node and its operator in the profile's `MeshNodeReputation`: 7 days, doubling per strike up to 30, kept in `mesh_node_reputation.json`, never shared. The mesh still promotes what it verified itself: connected, live, last good.

**Last good relays** ([call-path-resilience K015](../../projects/call-path-resilience/DECISIONS.md#k015--a-call-whose-relay-is-the-anchor-races-the-circuit-beside-the-direct-dial-the-app-remembers-its-last-good-relays)): the mesh remembers the relay that last carried a circuit for one run only, so a device that had just started parked on whichever seed was listed first (#577). `ProductMesh` keeps connectivity's one `SetOnRelayChosen` slot (calls hear through it: `CallStackDeps::set_on_relay_chosen`, H011) and notes each chosen relay in the profile's `LastGoodRelayStore` (`lib/people`: at most 3 PeerIds, most recent first, `mesh_last_good_relays.json`, saved on a worker, read at mesh start). The hop policy hands them back as `MeshHopPolicy::last_good_relays` — only those still a park target (a seed or a rendezvous relay, so trusted-only and a failed seed probe hold) and not demoted by this profile — and the park dials them first within each band (pp-cpp-mesh PEER_REACH § 5). They only reorder the walk, staggered as always: one that no longer answers costs 750 ms.

**Relay trust** (privacy T4): relays see both ends' addresses. `mesh.trusted_relays_only` (Me → Network → Trusted relays only) keeps every relay role — rendezvous parking, circuit dialing, punch introducers, call media hops — to the configured org seeds and Friendly contacts' nodes (`TrustedRelayPeerIds` / `KeepTrustedRelays` in `MeshHopPolicy`; applied in `MeshConnectivityWiring`, `CallHopRanking`, the hub's preferred bridge).

## Calls

Call **media** product path is **AMP** (voice-first): direct PeerLink channels and/or circuit nested Session + SoftMigrate `media_relay`. Wire-compat `call_sdp` / `call_ice` controls are ignored inbound; product does not send them. Code map: [CALLS.md](CALLS.md) · Amp: [CALL_MEDIA_CIRCUIT.md](../../projects/adp/CALL_MEDIA_CIRCUIT.md).

## Related

- [ARCHITECTURE.md](ARCHITECTURE.md) — overall shape  
- [P2P_MESSAGING.md](P2P_MESSAGING.md) — messaging  
- [LIBP2P_STREAMS.md](LIBP2P_STREAMS.md) — **legacy** stream framing notes (product underlay is Amp)  
- [LIBP2P_UPSTREAM.md](LIBP2P_UPSTREAM.md) — fork deltas  
- [COMPATIBILITY.md](../contracts/COMPATIBILITY.md) — wire/compat  
