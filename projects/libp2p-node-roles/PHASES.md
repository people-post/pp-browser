# Libp2p node roles — phases

## n0 — Project docs

- [x] README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md`
- [x] Common-tasks row in `AGENTS.md`
- [x] Cursor plan points at this folder; seed port **443**
- [x] Modes (client / node) + infrastructure vision (N008)
- [x] Role + capability checkboxes model (N009)
- [x] Monetization: per-capability pricing vs paid-jobs marketplace (N010)

## n1 — Role shell + bootstrap + Network UI

- [ ] `Libp2pConfig`: `bootstrap_peers` (default seed on TCP 443), `node_enabled`, listen default `/ip4/0.0.0.0/tcp/40123`
- [ ] `ConfigJson` + `config.json.example` (+ `PlatformDefaults` if used for seed)
- [ ] `ResolveLibp2pRole` (mobile → Client; desktop × `node_enabled`)
- [ ] `Libp2pHost` / `MessagingHub::StartLibp2p`: skip `host->listen` for Client; pass `listen_enabled`
- [ ] Register `bootstrap_peers` in `PeerSessionManager::RegisterEndpoint`
- [ ] `SessionConfigFromApp`: clamp via `Platform::IsMobile()` (not Android-only)
- [ ] Me → Network desktop **master** toggle for `node_enabled` + i18n — **no** capability or pricing UI yet
- [ ] Wire SettingsUiState / ApplyNetworkSettingsDraft / sync/flush/reset; hot-reload restarts libp2p
- [ ] Docs: `CONFIGURATION.md`, `P2P_MESSAGING.md`, `PLATFORMS.md` (point at N009/N010 for later)
- [ ] Unit tests: JSON round-trip; role resolver; Network draft apply
- [ ] Refresh CURRENT_STATE / README status when n1 ships

## n2 — Desktop DHT + checkbox (deferred)

- [ ] Wire fork Kademlia only when `Node && capabilities.dht`
- [ ] Bootstrap seed as DHT peer; never start DHT on Client / mobile
- [ ] Config `capabilities.dht` + Me → Network checkbox (nested under Node)
- [ ] No pricing required for DHT in v1 of this phase

## n3 — Circuit-relay + checkbox (deferred)

- [ ] Protocol absent from fork today — new fork work required
- [ ] `capabilities.circuit_relay` + checkbox; pricing optional (often volunteer)
- [ ] Until then: HTTP Brief relay remains fallback

## n4+ — Billable relays + pricing; chain; jobs marketplace (vision)

Each relay ships **protocol + capability flag + checkbox + pricing policy** together (N010):

- [ ] **Message relay** (`message_relay`) + `pricing.message_relay` (volunteer \| paid)
- [ ] **Audio relay** (`audio_relay`) + pricing
- [ ] **Video relay** (`video_relay`) + pricing
- [ ] **Blockchain node** (`blockchain`) — settlement/identity rails; not a substitute for relay pricing
- [ ] **Accept paid jobs** (`accept_paid_jobs`) — **secondary** marketplace; separate job schema; do not implement as the only monetization path

All hosting requires **Node** role (N008 / N009). Clients consume (and may pay).
