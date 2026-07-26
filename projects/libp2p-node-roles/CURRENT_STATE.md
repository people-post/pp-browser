# Libp2p node roles — current state

**Last updated:** 2026-07-26

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/libp2p-node-roles/` (n0) |
| ADRs | N001–N010 in [DECISIONS.md](DECISIONS.md) |
| Product model | Role + capability checkboxes (N009); **pricing on billable relays** primary, paid-jobs marketplace secondary (N010) |

## Agent trap (N010)

**Wrong:** treat `accept_paid_jobs` as “settle on chain” / the way relays charge users.  
**Right:** message/audio/video **relay** each get volunteer \| paid pricing; jobs marketplace is optional and separate. See [DESIGN § Monetization](DESIGN.md#monetization-n010--agents-read-this).

## Today (before n1)

| Area | State |
|------|-------|
| `Libp2pConfig` | `src/base/data/Config.h` — `listen_multiaddr` default `/ip4/127.0.0.1/tcp/40123`; session limits only. **No** `bootstrap_peers` / `node_enabled` / `capabilities` / `pricing` |
| Config JSON | `ConfigJson.cpp` serializes listen + session fields only |
| Me → Network | HTTP relay / directory / registration only (`NetworkSettingsSection`, `settings_sections.rml`) |
| Host start | `MessagingHub::StartLibp2p` always starts `Libp2pHost` with listen multiaddr; `Libp2pHost::Start` always calls `host_->listen` |
| Peer dials | `PeerSessionManager` dials **contact** multiaddrs via `RegisterContactEndpoints`; no bootstrap seed registration |
| DHT | Kademlia exists in `src/libp2p/fork` (protocol + example); **not** wired in integration |
| Circuit-relay | **Absent** from `src/libp2p/fork`; HTTP Brief relay remains the fallback path |
| Message / media / chain / pricing / jobs | **Not present** as peer-hosted node services |
| Mobile clamps | `SessionConfigFromApp` clamps only `PlatformKind::Android` — not `Platform::IsMobile()` |

## Key code touchpoints

| Path | Role |
|------|------|
| `src/base/data/Config.h` | `Libp2pConfig` defaults |
| `src/base/data/ConfigJson.cpp` | libp2p JSON |
| `src/feature/messaging/MessagingHub.cpp` | `StartLibp2p`, `SessionConfigFromApp` |
| `src/libp2p/integration/host/Libp2pHost.*` | Always listen today |
| `src/libp2p/integration/host/PeerSessionManager.*` | Endpoint register + dial |
| `src/feature/settings/NetworkSettingsSection.*` | Network UI (HTTP only) |
| `assets/views/settings_sections.rml` | Network section markup |
| `docs/ops/CONFIGURATION.md` | Documented listen example (0.0.0.0:40123) vs code default (127.0.0.1) |

## Next (n1)

1. Config fields + `ResolveLibp2pRole` (Client vs Node)
2. Skip listen for Client; register bootstrap peers
3. Me → Network desktop **master** toggle only (capabilities n2+; pricing with billable relays)
4. Docs + unit tests
5. Widen session clamps to `Platform::IsMobile()`

## Follow-ups

- **n2:** DHT + checkbox
- **n3:** Circuit-relay + checkbox
- **n4+:** Message/audio/video relay + **per-capability pricing** (N010); blockchain rails; later paid-jobs marketplace
