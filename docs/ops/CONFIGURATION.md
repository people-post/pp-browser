# Configuration (howto)

**Tier:** ops  
**Related:** [contracts/DATA_LAYOUT.md](../contracts/DATA_LAYOUT.md) (paths, on-disk tree, schema versions), [contracts/SERVICE_ENDPOINTS.md](../contracts/SERVICE_ENDPOINTS.md), [architecture/PLATFORMS.md](../architecture/PLATFORMS.md).

How to resolve, edit, and verify machine/profile settings. Normative disk layout lives in **DATA_LAYOUT**.

## Config resolution

1. `--config PATH`
2. `PP_BROWSER_CONFIG` environment variable
3. `{config_dir}/config.json`

There is **no** CWD `config.json` discovery. For local dev: `pp-browser --config config.json.example`.

**Sandbox backend:** pass `--sandbox` (or set `PP_BROWSER_SANDBOX=1`) to point Brief services at `https://www-en.qa.peoplepost.org` and use isolated config/data dirs (`pp-browser-sandbox` under XDG paths). Not persisted — production builds ignore it unless the flag/env is set.

Layering: `PlatformDefaults` → user config file → field-level merge (partial JSON is valid). Serialization lives in `src/base/data/ConfigJson.*` (`Object` encode/decode with `DeepMergeObject`).

### `pp-node` deploy overlays

Headless **`pp-node`** uses the same config file schema, then applies deploy env, then CLI:

**CLI flags → environment → config file → defaults**

| Variable | Maps to | Notes |
|----------|---------|--------|
| `PP_BROWSER_PIN` | profile unlock | Required (or `--pin`). Secret — never bake into images. |
| `PP_BROWSER_CONFIG` | config file path | Or `--config` |
| `PP_NODE_DATA_DIR` | `data_dir` | Volume mount path |
| `PP_NODE_AMP_UDP_PORT` | `libp2p.amp_udp_port` | Org seed should pin **443** to match default ADP bootstrap |
| `PP_NODE_BOOTSTRAP_PEERS` | `libp2p.bootstrap_peers` | Comma-separated **ADP** multiaddrs (`/udp/…/adp/1.0.0/p2p/…`) |
| `PP_NODE_CAP_CIRCUIT_RELAY` | `capabilities.circuit_relay` | `true`/`1`/`yes`/`on` or `false`/`0`/`no`/`off` |
| `PP_NODE_CAP_MEDIA_RELAY` | `capabilities.media_relay` | Same bool forms |
| `PP_NODE_ADVERTISE_MULTIADDRS` | `libp2p.advertise_multiaddrs` | Comma-separated **public** multiaddrs for directory publish (never `0.0.0.0`) |
| `PP_NODE_MESH_PUBLISH` | `libp2p.mesh_publish` | Register/renew as `entity_kind=mesh_node` (N027). Default on when advertise list is non-empty |
| `PP_NODE_REGISTRATION_BASE_URL` | `registration.base_url` | Mesh directory register/renew HTTP base (e.g. sandbox `https://www-en.qa.peoplepost.org/api/relay`) |
| `PP_NODE_IDENTITY_SEED` | deterministic identity | ≥32-byte hex master seed; HKDF `pp-node-identity-v1` → device ML-DSA + account ML-DSA + account ML-KEM. Empty volume mints stably; existing `identity.enc` **fail-closed** on mismatch |
| `PP_NODE_PROFILE` | active profile id | Or `--profile` |
| `PP_NODE_STATUS_ADDR` | status HTTP bind | Default `127.0.0.1:18518`; empty disables. Set `0.0.0.0:18518` (or a host IP) to expose for console/probes — ADDR alone is enough |
| `PP_NODE_STATUS_TOKEN` | status Bearer token | Optional; when set, required for both `/healthz` and `/status` |

JSON remains the durable seed profile (caps, budgets, pricing). Env is for secrets and per-instance overrides (Compose/Kubernetes). Implementation: `src/app/node/NodeEnvOverlay.*`.

**Dogfood initiation floor (P001):** top-level `initiation_floor` (integer `pp_credit` minor units, default `0`) seeds `LocalIdentity.initiation_floor` when the identity value is still `0`. No Me UI yet — set in `config.json` for testing. Older directory/register servers that omit the field are treated as `0`.

## Runtime session state

After bootstrap, [`Application`](../../src/app/Application.h) owns the live [`SessionStore`](../../src/base/data/SessionStore.h) (`BootstrapResult`: config, profile prefs, paths). Settings and chat read/write through injected store / ports; saves reload from disk before notifying listeners.

**Disk DTOs vs service slices:** `AppConfig` / `ProfilePreferences` are persistence schemas. Hot-reload does **not** pass those blobs straight into services. [`ConfigApplyBridge`](../../src/app/ConfigApplyBridge.h) (composition root) projects nested service types and calls `Apply` only when a slice changes. Diagrams: [architecture/RUNTIME_COMPOSITION.md](../architecture/RUNTIME_COMPOSITION.md).

| Disk DTO | Projector | Service slice | Apply |
|----------|-----------|---------------|-------|
| `AppConfig` | `MessagingHub::ProjectNetwork` | `MessagingHub::NetworkConfig` | `MessagingHub::Apply` |
| `AppConfig` | `ChatController::ProjectAgent` | `ChatController::AgentConfig` | `ChatController::Apply` |
| `ProfilePreferences` | `MessagingHub::ProjectPolicy` | `MessagingHub::PolicyPrefs` | `MessagingHub::Apply` |
| `ProfilePreferences` | `MessagingHub::ProjectNotifications` | `MessagingHub::NotificationPrefs` | `MessagingHub::Apply` |
| `ProfilePreferences` | `ShellHost::ProjectChrome` | `ShellHost::ChromePrefs` | Theme + `ShellHost::Apply` (materials) |
| `ProfilePreferences` | `LocalizationService::Project` | `LocalizationService::Prefs` | `LocalizationService::Apply` (UI chrome via language listeners) |

Slice types are **nested on the owning service class**. Settings section flush only writes disk DTOs. Cross-module access (session store, identity, locales, appearance, reachability, PIN status, register / rotate / UPnP / clear undelivered / reset profile) uses [`SettingsCommands`](../../src/feature/settings/SettingsCommands.h) / [`ProfileIdentityView`](../../src/base/people/ProfileIdentityView.h) / [`SettingsPortsViews`](../../src/feature/settings/SettingsPortsViews.h) — ports filled via `SettingsController::BindCommands` from `Application` (implementations call [`MessagingHub`](../../src/feature/messaging/MessagingHub.h), `SessionStore`, etc.); UI re-syncs `SettingsUiState` after commands. Settings does **not** hold a messaging pointer (`BindMessaging` / `Hub()` removed).

| SessionStore listener | Used by |
|-----------------------|---------|
| `AddConfigListener` | `ConfigApplyBridge` (messaging network) + `ChatController` (agent/LLM) |
| `AddProfilePrefsListener` | `ConfigApplyBridge` (policy, notifications, chrome, locale) |
| Theme / appearance / language / chrome field listeners | Still available; app hot-reload prefers slice projection via prefs listener |

## LLM presets

`config.json` may include `llm.preset`: `"brief"`, `"cloud"`, `"ollama"`, or `"custom"`. Preset metadata and apply logic live in `src/base/data/LlmPreset.*`. Legacy files without `preset` infer it once from `base_url`.

`NormalizeLlmConfig` is the single write-boundary translator: it turns preset intent into precise `base_url`, `model` (wire id), and auth flags. Settings flush and config load both call it. Runtime Brief chat only overlays `identity.brief_llm_api_key` — it does not re-interpret model/preset. See **Settings: normalize at write boundary** below.

## Settings: normalize at write boundary

Machine and profile settings should follow the same shape as LLM config:

| Layer | Responsibility |
|-------|----------------|
| UI / draft | Capture **user intent** (preset, toggles, free-text overrides) |
| **One normalizer** (per settings domain) | Translate intent → **precise persisted fields** |
| Disk (`config.json` / `preferences.json`) | Store only precise values |
| Runtime readers | Interpret fields literally; inject secrets from vault/env if needed |

**Do:**

- Put translation in one function (e.g. `NormalizeLlmConfig`, future `NormalizeNetworkConfig`)
- Call it on settings flush **and** on load (heal dirty / partial / legacy files)
- On preset-like UI changes, update draft defaults (e.g. model → `DefaultModelForPreset`) before flush so the user sees what will be saved
- Keep vault/env secrets out of the normalizer’s persisted outputs when the domain says so (Brief API key → `identity.enc`)

**Don’t:**

- Re-encode the same denylist / “fixup” in `ChatController`, `LlmClient`, and settings
- Persist ambiguous values (`model: "brief"`) and hope every reader guesses

When adding a new Me-tab section, add its normalizer next to the domain types under `src/base/data/` or `src/feature/settings/`, wire it from that section’s `Flush` + the relevant load path, and document the precise on-disk fields here.

## Theme and appearance (runtime)

**Appearance (light/dark):** `profiles/{id}/preferences.json` → `appearance` (`system`, `light`, or `dark`). System follows `SDL_GetSystemTheme` and live-updates on `SDL_EVENT_SYSTEM_THEME_CHANGED`.

**Language (UI):** `preferences.json` → `language` (`system`, `en`, or `zh-Hans`). `system` follows `SDL_GetPreferredLocales` and picks the first shipped catalog match (else English). Changing language in Me → Appearance applies immediately via `LocalizationService` + shell remount. Catalogs live under `assets/locales/`.

**Compact chrome materials:** `preferences.json` → `reduce_transparency` (Me → Appearance; opaque shell, no backdrop frost) and `compact_chrome_frost` (default true; disable frost tier via JSON for dogfood). Schema v8.

**Reachability nudge ack:** `preferences.json` → `reachability_nudge_acked_status` (`outbound_only` / `blocked`, or empty). Schema v9. Cleared when inbound becomes reachable so a later regression can show the Me / Network attention dots again.

**PIN state:** `preferences.json` → `pin_is_default` — see [DATA_LAYOUT](../contracts/DATA_LAYOUT.md) and [AT_REST_ENCRYPTION](../contracts/AT_REST_ENCRYPTION.md).

See [ui/UI_DESIGN_SYSTEM.md](../ui/UI_DESIGN_SYSTEM.md) for tokens and component classes.

## In-app settings (Me tab)

Open **Me** from the nav rail (person icon). The Me tab shows an **identity card** (nickname, relay ID, Copy ID / Share / Register) above a **Preferences** list → detail layout:

| Section | Persists to | Scope |
|---------|-------------|-------|
| Profile (Me card) | `identity.enc` (+ `vault.bin`) | profile |
| Assistant | `config.json` | machine |
| Integrations | `config.json` | machine |
| Network | `config.json` | machine |
| Appearance | `preferences.json` (`appearance`, `language`) | profile |
| Security | `vault.bin` + `preferences.json` (`pin_is_default`) | profile |
| Storage | paths + profile size; reset wipes profile dir | — |

On tab entry, [`SettingsController`](../../src/feature/ui/SettingsController.cpp) reloads from disk via `SessionStore::ReloadFromDisk()` so the UI matches persisted files. Changes **auto-save per block**: select fields save immediately; text fields debounce ~500ms. Pending changes flush before switching sections or leaving the tab. Config sections apply through [`SettingsLogic`](../../src/feature/settings/SettingsLogic.cpp), write to disk, and reload into `SessionStore`. Runtime apply is owned by [`ConfigApplyBridge`](../../src/app/ConfigApplyBridge.h) (service slices) and `ChatController` (LLM/agent config listener) — not by settings controllers calling hubs directly.

### Machine config keys (`config.json`)

```json
{
  "llm": {
    "preset": "brief",
    "base_url": "https://www.brief.global/api/llm/v1",
    "model": "xai",
    "require_api_key": false
  },
  "promoted_mcp": { "url": "https://www.brief.global/mcp" },
  "mcp_servers": [
    { "id": "my-tooling", "url": "https://example.com/mcp", "enabled": true }
  ],
  "search": { "provider": "duckduckgo" },
  "relay": { "base_url": "https://www.brief.global/api/relay" },
  "directory": { "base_url": "https://www.brief.global/api/relay" },
  "registration": { "base_url": "https://www.brief.global/api/relay" },
  "libp2p": {
    "node_enabled": true,
    "mesh_enabled": true,
    "amp_udp_port": 0,
    "bootstrap_peers": [
      "/ip4/3.208.41.58/udp/443/adp/1.0.0/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"
    ]
  }
}
```

- **`llm`** — default preset is **Brief** (API key issued on Profile registration, stored in `identity.enc`). **Cloud**, **Ollama**, and **Custom** remain available in Me → Assistant.
- **`promoted_mcp`** — primary MCP endpoint (feeds, promoted infra tools). Blank URL uses [`PlatformDefaults`](../../src/base/data/PlatformDefaults.cpp).
- **`mcp_servers`** — additional MCP servers (custom tool bucket). Legacy `"mcp"` key loads into `promoted_mcp`.
- **`relay` / `directory` / `registration`** — HTTP endpoints; platform default is Brief. Empty `base_url` coalesces to platform defaults (not mocks). See [SERVICE_ENDPOINTS.md](../contracts/SERVICE_ENDPOINTS.md).
- **`libp2p`** — mesh role and Amp underlay policy. `node_enabled` (desktop; ignored on mobile) selects Node vs Client hosting posture ([p2p-mesh N001](../../projects/p2p-mesh/DECISIONS.md)). **`mesh_enabled`** (default **true**) is required for the peer mesh: Amp UDP + MSH + channels ([adp D10](../../projects/adp/PHASES.md)). Amp bind/start failure **fails mesh start** (no TCP underlay fallback). Set `mesh_enabled=false` to leave peer mesh off. Optional **`amp_udp_port`** (0 = ephemeral; org seed should pin **443**). Empty `bootstrap_peers` fills the Brief ADP seed (`/udp/443/adp/1.0.0/…`) for SoftMigrate + dial-back. LAN mDNS TXT includes `amp_udp=` so peers can build ADP multiaddrs. Org **`pp-node`** hosts Amp circuit/media-relay when capabilities are on. Me → Network shows Help-the-network, Amp listen, and Inbound via Amp dial-back (D8). Contacts may store dialable ADP `multiaddrs` (`/ip4/…/udp/…/adp/1.0.0/p2p/<PeerId>`). Prefer the modern **`mesh`** key (same schema); **`libp2p`** is a legacy alias at load time.

### Mesh DHT (n2)

Spec: [MESH_DHT.md](../contracts/MESH_DHT.md), ADR [N028](../../projects/p2p-mesh/DECISIONS.md#n028--amp-native-mesh-dht-find_peer-v1). Implemented through **n2-hard**.

| Field | Default | Notes |
|-------|---------|-------|
| `mesh.capabilities.dht` | `false` | Node-only; mobile ignores. UI checkbox (N008). |
| `mesh.dht.record_ttl_seconds` | `3600` | Self `peer_routing` record TTL; re-publish at ttl/2 when enabled. |
| `mesh.dht.find_peer_timeout_ms` | `5000` | Consumer FIND_PEER timeout. |
| `mesh.dht.max_concurrent_lookups` | `4` | In-flight lookup cap. |
| `mesh.dht.k_bucket_size` | `20` | Kademlia *k*; wire default matches [MESH_DHT.md](../contracts/MESH_DHT.md). |
| `mesh.dht.inbound_ops_per_peer_per_window` | `60` | Inbound FIND_PEER/STORE grants per remote peer per window. |
| `mesh.dht.inbound_rate_window_seconds` | `60` | Sliding window for inbound rate limit. |
| `mesh.dht.soft_reputation_penalty_threshold` | `3` | Bad FIND_PEER replies before cooldown. |
| `mesh.dht.soft_reputation_cooldown_seconds` | `300` | Skip query peer after soft-reputation penalty. |

DHT complements [mesh directory](../../projects/p2p-mesh/MESH_DIRECTORY.md) (n-dir): bootstrap ∪ directory cache, never bypasses hop policy. pp-ledger fleet does **not** use this DHT — see [platform-integration](../../../pp-ledger/docs/platform-integration.md).

Enter an **API key** directly in Me → Assistant (saved to `config.json`) or use **API key env var** for desktop-style env lookup when using Cloud/Custom. Leaving the password field blank on save keeps an existing saved API key. Default preset is **Brief** (key from Profile registration); **Ollama (localhost)** remains available for local dev.

### Verify settings persistence (manual)

```bash
pp-browser --config /tmp/pp-test-config.json
# Me → Assistant → change model → wait briefly → back → reopen Me
jq .llm.model /tmp/pp-test-config.json
```

The on-disk model should match what you set.

## Platform layer

Shared abstractions under `src/base/platform/` — see [PLATFORMS.md](../architecture/PLATFORMS.md):

- `IPathProvider` / `IAssetLocator` — desktop paths vs APK/bundle assets
- `AssetIO` / `SdlAssetFileInterface` — unified bundle reads for UI and RmlUi
- `PlatformDefaults` — Brief network + Brief LLM on all platforms
- `PlatformNavigation` — Escape and Android back → `ShellHost::HandleDismiss()`
- `AppLifecycle` — background IO pause, agent cancel, P2P poll guard
- `ICredentialStore` / `EnvCredentialStore` — env-backed keys; Keystore deferred

## Environment variables

| Variable | Purpose |
|----------|---------|
| `PP_BROWSER_CONFIG` | Explicit config file path |
| `PP_BROWSER_SANDBOX` | When truthy, same as `--sandbox` (sandbox backend + isolated dirs) |
| `PP_BROWSER_PIN` | Profile unlock PIN (`pp-node` / automation) |
| `PP_BROWSER_LLM_MODEL` | Default Brief model when no config file |
| `PP_NODE_*` | Headless node deploy overlays — see [pp-node deploy overlays](#pp-node-deploy-overlays) |
| `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` | Linux path overrides |

API keys can be set inline in Settings/config (`llm.api_key`) or via `api_key_env` resolved through `ICredentialStore`.
