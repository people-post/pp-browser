# P2P mesh — current state

**Last updated:** 2026-08-01

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-mesh/` (n0; renamed from `libp2p-node-roles`) |
| ADRs | N001–**N025** in [DECISIONS.md](DECISIONS.md) (N025 = mobile call-scoped listen — planned) |
| Product model | Role/caps; pricing; `pp-node`; reachability; IPv6/UPnP; contact-first; listen **18517** + busy fallback (N016) |
| Networking doctrine | [NETWORKING.md](../../docs/architecture/NETWORKING.md) — HTTP + libp2p; calls consume fabric (V026) |
| **n1** | Role shell + bootstrap + Me → Network master toggle (see below) |
| **np** | Headless `pp-node` + shared `NodeRuntime` + dial-back protocol (see below) |
| **nr** | Reachability status + Connection card + guided help + `pp-node --status` (see below) |
| **nu** | IPv6 listen candidates + UPnP (miniupnpc) + Connection card actions (see below) |
| **n3** | Custom `/pp-browser/circuit-relay/1.0.0` + `capabilities.circuit_relay` + UI checkbox (see below) |
| **nf** | Contact-first circuit preference + provider admission (see below) |
| **n4-media** | Blind `media_relay` + N021 framing + quote/attach + closed-set pick helpers (see below) |

## n1 in code

| Area | State |
|------|-------|
| `Libp2pConfig` | `node_enabled`, `bootstrap_peers` (seed tcp/443), listen default **18517** |
| Role | `ResolveLibp2pRole` — mobile Client; desktop × `node_enabled` |
| Listen | Client skips `host->listen`; Node tries **18517–18526** then `/tcp/0`; persist bound addr |
| Errors | `MessagingHub::LastLibp2pError` surfaced in Me → Network |
| UI | Desktop **Help the network** toggle + actual listen multiaddr |
| Tests | `Libp2pRole` helpers + ConfigJson / settings merge |

## np in code

| Area | State |
|------|-------|
| Platform split | `pp_base_platform_core` (paths/OS/env, no SDL/RmlUi) vs GUI `pp_base_platform` |
| Shared runtime | `libp2p/integration/host/NodeRuntime` — host start/stop, listen candidates, bootstrap, tick |
| Busy-port | `ListenBusyPolicy::FailLoud` (pp-node default) vs `DesktopFallback` (GUI) |
| Binary | `pp-node` (`src/app/node/`) — PIN unlock, force Node, signal wait |
| Dial-back | `/pp-browser/dial-back/1.0.0` (`DialBackService`) — seed probes client listen addrs |
| Packaging | `packaging/pp-node/` systemd + Dockerfile sketches + config example |
| Tests | FailLoud candidates; two-host dial-back LAN probe |

## nr in code

| Area | State |
|------|-------|
| Probe | `ReachabilityService` — seed dial + dial-back + IP classification |
| Status | Reachable / Outbound only / Blocked / Unknown / Checking |
| GUI | Me → Network Connection card; soft shell banner; guided help sheets |
| Ops | `pp-node --status` JSON |
| i18n | en + zh-Hans reachability strings |
| Tests | `reachability_test.cpp` |

## nu in code

| Area | State |
|------|-------|
| IPv6 | `EnumerateGlobalIpv6Addresses` + extra listen candidates on Node start |
| UPnP | Vendored `miniupnpc`; `TryUpnpTcpPortMapping`; auto-try once on Node enable |
| UI | **Open port on router…** + re-test; skip UPnP on public listen IPs |
| Fallback | Outbound-only help sheet (N012 manual forward copy) |

## n3 in code

| Area | State |
|------|-------|
| Protocol | `/pp-browser/circuit-relay/1.0.0` stream bridge — **single-hop today**; multi-hop v2 planned ([MULTI_HOP_CIRCUIT.md](../media-hop-reachability/MULTI_HOP_CIRCUIT.md)) |
| Config | `libp2p.capabilities.circuit_relay` + JSON round-trip |
| UI | **Circuit relay** checkbox under Node (hot refresh via `RefreshMeshCapabilities`) |
| Seed | `packaging/pp-node/config.json.example` enables `circuit_relay: true` |
| Auto-route | **nf** — `MessagingHub::RequestCircuitBridgePreferred` |

## nf in code

| Area | State |
|------|-------|
| Config | `prefer_contacts_for_routing` (default on) |
| Pick | `MeshHopPolicy` — contacts → seed for circuit (`OrderCircuitHops`) |
| API | `MessagingHub::RequestCircuitBridgePreferred` |
| Provider | Circuit (and media) admission prefers contacts on volunteer desktop when contacts known; org seed with empty contacts serves all |
| UI | **Prefer contacts for routing** toggle |

## n4-media in code

| Area | State |
|------|-------|
| Protocol | `/pp-browser/media-relay/1.0.0` (`MediaRelayService`) |
| Framing | N021 binary frames: `stream_id \| channel_id \| channel_type \| seq \| mark` + opaque payload |
| QoS | `reliable_ordered`, `latest_lossy`, `best_effort` |
| Session | quote → accept → attach; subscribe `(stream_id, channel_id)`; ↑/↓ budget defaults; volunteer rate 0 |
| Auth stub | `auth` must equal `call_id` (a4 will supply roster proof) |
| Config | `capabilities.media_relay` **default on**; `pricing.media_relay`; `media_relay_budget` |
| Pick helpers | `RankMediaHopsEscalating` (N023 ns1) — contacts ∪ seed; call coordinator wires a4 |
| Hosts | `pp-node` + desktop Node checkbox default on |
| Tests | frame codec; hop policy; two-host quote/attach/fan-out |

## Agent traps

| Wrong | Right |
|-------|-------|
| `accept_paid_jobs` = relay monetization | Per-relay pricing (N010) |
| Org seed = GUI `--headless` | **`pp-node`** (N011) |
| “Behind firewall” as hard fact | Reachability status + soft help (N012) |
| Manual port-forward only | Prefer **IPv6 + UPnP**, then manual (N013) |
| Pick random public relay first | **N023** scope escalate + **N020** / **V023** scorer (contacts∪seed short term) |
| Hardcoded N014 stages for media | Outer scope bands + inner N020 scorer — not fixed stage list |
| Implement DHT right after n1 | Follow **N015** order (circuit/reachability before DHT) |
| Always bind 18517 or die silently | Desktop: fallback range + persist (N016); `pp-node`: fail loud |
| Link `MessagingHub` into `pp-node` | Thin `NodeRuntime` + identity/crypto only |
| Silent port hop on org seed | Fail loud unless `--listen-fallback` |
| libp2p circuit-relay v2 in fork | Custom pp-browser circuit-relay protocol (n3) |
| Relay decodes Opus/H264 | Blind forward + `channel_type` only (N021) |

## ns in code (N023 — partial)

| Area | State |
|------|-------|
| `RelayScope` + escalate ranker | `RelayScope.h`, `RankMediaHopsEscalating` in `MeshHopPolicy` |
| Provider cap from reachability | `ApplyMeshAdmissionPolicies` → `serve_scope_mask` |
| Bridge score / LAN mDNS | Deferred (ns2); multi-hop circuit plan [ns3](PHASES.md#ns3--multi-hop-circuit-policy) / [N024](DECISIONS.md#n024--immediate-relay-as-service-broker) |
| Household contact tag | Deferred |

## Still not done

| Area | State |
|------|-------|
| Call consumer (a4) | Soft-migrate + V024 shared policy — [p2p-av-calls](../p2p-av-calls/) |
| Peer message_relay | Deferred (N017); HTTP Brief remains |
| Open public / paid settle UI | **N020 mid** — pricing regulates; not revenue-first |
| Bonds / reputation / anti-capture | **N020 long** |
| DHT | **n2** (later per N015) |
| Mobile call-scoped listen | **nm** — N025 planned, not in code |

## Next

1. **a4** — group / ICE-fail via `media_relay`; shared V024 adaptation (single video layer OK)  
2. Curated public / paid regulation / **n2 DHT** later  

## Follow-ups

See [PHASES.md](PHASES.md) and [DESIGN § Relay selection and scope](DESIGN.md#relay-selection-and-scope).

**Calls:** [p2p-av-calls](../p2p-av-calls/) **a4** — V020–V024. Shared adaptive policy (1:1 P2P + SFU); N021 framing on hop only.
