# Configuration (howto)

**Tier:** ops  
**Related:** [contracts/DATA_LAYOUT.md](../contracts/DATA_LAYOUT.md) (paths, on-disk tree, schema versions), [contracts/SERVICE_ENDPOINTS.md](../contracts/SERVICE_ENDPOINTS.md), [architecture/PLATFORMS.md](../architecture/PLATFORMS.md).

How to resolve, edit, and verify machine/profile settings. Normative disk layout lives in **DATA_LAYOUT**.

## Config resolution

1. `--config PATH`
2. `PP_BROWSER_CONFIG` environment variable
3. `{config_dir}/config.json`

There is **no** CWD `config.json` discovery. For local dev: `pp-browser --config config.json.example`.

Layering: `PlatformDefaults` → user config file → field-level merge (partial JSON is valid). Serialization lives in `src/base/data/ConfigJson.*` (nlohmann `to_json` / `from_json` with deep merge).

## Runtime session state

After bootstrap, a single [`SessionStore`](../../src/base/data/SessionStore.h) owns the live `BootstrapResult` (config, profile prefs, paths). Settings and chat read/write through it; saves reload from disk before notifying listeners.

**Disk DTOs vs service slices:** `AppConfig` / `ProfilePreferences` are persistence schemas. Hot-reload does **not** pass those blobs straight into services. [`ConfigApplyBridge`](../../src/app/ConfigApplyBridge.h) (composition root) projects nested service types and calls `Apply` only when a slice changes:

| Disk DTO | Projector | Service slice | Apply |
|----------|-----------|---------------|-------|
| `AppConfig` | `MessagingHub::ProjectNetwork` | `MessagingHub::NetworkConfig` | `MessagingHub::Apply` |
| `ProfilePreferences` | `MessagingHub::ProjectPolicy` | `MessagingHub::PolicyPrefs` | `MessagingHub::Apply` |
| `ProfilePreferences` | `MessagingHub::ProjectNotifications` | `MessagingHub::NotificationPrefs` | `MessagingHub::Apply` |
| `ProfilePreferences` | `ShellHost::ProjectChrome` | `ShellHost::ChromePrefs` | Theme + `ShellHost::Apply` (materials) |
| `ProfilePreferences` | `LocalizationService::Project` | `LocalizationService::Prefs` | `LocalizationService::Apply` + shell remount |
| `AppConfig` | (chat still listens to full config) | — | `ChatController::ApplyRuntimeConfig` |

Slice types are **nested on the owning service class**. Settings section flush only writes disk DTOs.

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
    "model": "grok-4-1-fast-reasoning",
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
    "listen_multiaddr": "/ip4/0.0.0.0/tcp/18517",
    "bootstrap_peers": [
      "/ip4/3.208.41.58/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"
    ],
    "max_connections": 48,
    "max_concurrent_dials": 6,
    "dial_timeout_ms": 8000,
    "idle_ttl_ms": 180000,
    "dial_failure_backoff_ms": 30000
  }
}
```

- **`llm`** — default preset is **Brief** (API key issued on Profile registration, stored in `identity.enc`). **Cloud**, **Ollama**, and **Custom** remain available in Me → Assistant.
- **`promoted_mcp`** — primary MCP endpoint (feeds, promoted infra tools). Blank URL uses [`PlatformDefaults`](../../src/base/data/PlatformDefaults.cpp).
- **`mcp_servers`** — additional MCP servers (custom tool bucket). Legacy `"mcp"` key loads into `promoted_mcp`.
- **`relay` / `directory` / `registration`** — HTTP endpoints; platform default is Brief. Empty `base_url` coalesces to platform defaults (not mocks). See [SERVICE_ENDPOINTS.md](../contracts/SERVICE_ENDPOINTS.md).
- **`libp2p`** — mesh role and host policy. `node_enabled` (desktop; ignored on mobile) selects Node vs Client ([p2p-mesh N001](../../projects/p2p-mesh/DECISIONS.md)). Preferred desktop Node listen is `/ip4/0.0.0.0/tcp/18517` ([N003](../../projects/p2p-mesh/DECISIONS.md)); if busy, the app tries **18517–18526** then optional ephemeral and persists the actual multiaddr ([N016](../../projects/p2p-mesh/DECISIONS.md)). Empty `bootstrap_peers` fills the Brief seed (`/ip4/3.208.41.58/tcp/443/p2p/…`). Org **`pp-node`** fails loud on its configured port (often 443) unless `--listen-fallback` ([N011](../../projects/p2p-mesh/DECISIONS.md)). Me → Network shows the Help-the-network toggle and the **actual** listen address. Contacts may store dialable `multiaddrs` (must include `/p2p/<PeerId>`).

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
| `PP_BROWSER_LLM_MODEL` | Default Brief model when no config file |
| `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` | Linux path overrides |

API keys can be set inline in Settings/config (`llm.api_key`) or via `api_key_env` resolved through `ICredentialStore`.
