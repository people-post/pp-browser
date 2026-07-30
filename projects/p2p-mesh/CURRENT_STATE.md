# P2P mesh — current state

**Last updated:** 2026-07-30

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-mesh/` (n0; renamed from `libp2p-node-roles`) |
| ADRs | N001–N019 in [DECISIONS.md](DECISIONS.md) |
| Product model | Role/caps; pricing; `pp-node`; reachability; IPv6/UPnP; contact-first; listen **18517** + busy fallback (N016) |
| **n1** | Role shell + bootstrap + Me → Network master toggle (see below) |
| **np** | Headless `pp-node` + shared `NodeRuntime` + dial-back protocol (see below) |
| **nr** | Reachability status + Connection card + guided help + `pp-node --status` (see below) |
| **nu** | IPv6 listen candidates + UPnP (miniupnpc) + Connection card actions (see below) |
| **n3** | Custom `/pp-browser/circuit-relay/1.0.0` + `capabilities.circuit_relay` + UI checkbox (see below) |

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
| Protocol | `/pp-browser/circuit-relay/1.0.0` stream bridge (integration layer, not libp2p v2) |
| Config | `libp2p.capabilities.circuit_relay` + JSON round-trip |
| UI | **Circuit relay** checkbox under Node (hot refresh via `RefreshMeshCapabilities`) |
| Seed | `packaging/pp-node/config.json.example` enables `circuit_relay: true` |
| Auto-route | Deferred to **nf** (manual bridge API on `CircuitRelayService`) |

## Agent traps

| Wrong | Right |
|-------|-------|
| `accept_paid_jobs` = relay monetization | Per-relay pricing (N010) |
| Org seed = GUI `--headless` | **`pp-node`** (N011) |
| “Behind firewall” as hard fact | Reachability status + soft help (N012) |
| Manual port-forward only | Prefer **IPv6 + UPnP**, then manual (N013) |
| Pick random public relay first | **Contacts first**, then seed, then public (N014) |
| Implement DHT right after n1 | Follow **N015** order (circuit/reachability before DHT) |
| Always bind 18517 or die silently | Desktop: fallback range + persist (N016); `pp-node`: fail loud |
| Link `MessagingHub` into `pp-node` | Thin `NodeRuntime` + identity/crypto only |
| Silent port hop on org seed | Fail loud unless `--listen-fallback` |
| libp2p circuit-relay v2 in fork | Custom pp-browser circuit-relay protocol (n3) |

## Still not done

| Area | State |
|------|-------|
| Contact-first relay routing | **nf** (not implemented; SFU pick ranking TBD) |
| Blind `media_relay` + ↑/↓ budgets + quote schema | **n4-media** (N017–N019) — unblocks calls a4 |
| Peer message_relay | Deferred (N017); HTTP Brief remains |
| Paid pricing UI / settle | Deferred; quote/ceiling designed now (N019); rate 0 volunteer first |
| DHT | **n2** (later per N015) |

## Next

1. **nf** — thin contact-first for circuit (N014); SFU pick **scorer TBD** (after N019)  
2. **n4-media** — blind `media_relay` + ↑/↓ quote path (N018/N019)  
3. Peer message_relay / paid UI / **n2 DHT** later  

## Follow-ups

See [PHASES.md](PHASES.md) and [DESIGN § Preferred delivery order](DESIGN.md#preferred-delivery-order-n015).

**Calls:** [p2p-av-calls](../p2p-av-calls/) **a4** needs blind **`media_relay`** (V020–V022). ↑/↓ budgets + no-surprise quotes; pick rank still open.
