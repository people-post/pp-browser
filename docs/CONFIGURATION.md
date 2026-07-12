# Configuration

pp-browser stores machine-wide connection settings separately from per-profile identity data. No first-run wizard is required: built-in platform defaults apply when no config file exists.

## Terminology

| Term | Meaning |
|------|---------|
| **Profile** | Network identity namespace (keypair, contacts, conversations) |
| **Session / thread** | One conversation in the sidebar (not an account) |
| **Machine** | One app install under one OS user |

## Paths (desktop)

| Scope | Linux | macOS | Windows |
|-------|-------|-------|---------|
| Config | `$XDG_CONFIG_HOME/pp-browser/config.json` or `~/.config/pp-browser/` | `~/Library/Application Support/pp-browser/` | `%APPDATA%/pp-browser/` |
| Data | `$XDG_DATA_HOME/pp-browser/` or `~/.local/share/pp-browser/` | `~/Library/Application Support/pp-browser/data/` | `%LOCALAPPDATA%/pp-browser/` |

Override data root with `data_dir` in config (supports `~` expansion).

## On-disk layout

```
{config_dir}/config.json
{data_dir}/profiles.json
{data_dir}/machine.json
{data_dir}/profiles/{id}/
  manifest.json
  preferences.json
  vault.bin                 # PIN-wrapped DEK (created on first secrets unlock)
  identity.enc              # identity JSON under DEK AEAD
  contacts.json
  threads/
    profile.db              # thread catalog, outbox, chat_targets (PSK columns encrypted)
    {thread_id}/
      thread.db             # messages, memory, sync_state (plaintext — D048)
      blobs/                # attachment placeholder
```

**PIN:** Interactive unlock/create uses an in-app modal (`PinGateController`). `--pin` / `PP_BROWSER_PIN` remain optional for tests/CI.

- **No vault:** first secrets use shows a three-way chooser — set a custom PIN, **Just continue** (app default `123456`, stored as `pin_is_default: true` in `preferences.json`), or dismiss (“Not now”) and retry later.
- **Vault + `pin_is_default`:** silent unlock at bootstrap and after UI load; no modal unless unlock fails.
- **Vault + custom PIN:** blocking unlock modal after UI load (no cancel).
- **Change PIN:** Me → Security when unlocked; clears `pin_is_default`.

Forgotten PIN → wipe the profile directory. See [AT_REST_ENCRYPTION.md](AT_REST_ENCRYPTION.md).

Legacy flat `threads/index.json` and `{thread_id}.json` are removed on first run after the SQLite migration (development wipe — see [chat-storage D016](projects/chat-storage-and-memory/DECISIONS.md)). Plaintext `identity.json` is not migrated — wipe the profile and recreate.

Phase 1 ships a single `default` profile. Use `--profile NAME` for dev isolation (no account-switcher UI yet).

## Config resolution

1. `--config PATH`
2. `PP_BROWSER_CONFIG` environment variable
3. `{config_dir}/config.json`

There is **no** CWD `config.json` discovery. For local dev: `pp-browser --config config.json.example`.

Layering: `PlatformDefaults` → user config file → field-level merge (partial JSON is valid). Serialization lives in `src/base/data/ConfigJson.*` (nlohmann `to_json` / `from_json` with deep merge).

## Runtime session state

After bootstrap, a single [`SessionStore`](../src/base/data/SessionStore.h) owns the live `BootstrapResult` (config, profile prefs, paths). Settings and chat read/write through it; saves reload from disk before notifying listeners.

| Listener | Trigger |
|----------|---------|
| Config | `SessionStore::SaveConfig` → `ChatController` / `AgentSession::Configure` |
| Theme | `SessionStore::SaveProfilePrefs` → `Theme::LoadBase` |

## LLM presets

`config.json` may include `llm.preset`: `"cloud"`, `"ollama"`, or `"custom"`. Preset metadata and apply logic live in `src/base/data/LlmPreset.*`. Legacy files without `preset` infer it once from `base_url`.

## Theme and appearance

**Appearance (light/dark):** `profiles/{id}/preferences.json` → `appearance` (`system`, `light`, or `dark`). System follows `SDL_GetSystemTheme` and live-updates on `SDL_EVENT_SYSTEM_THEME_CHANGED`.

**PIN state:** `preferences.json` → `pin_is_default` (boolean, schema v3). Set when the user chooses “Just continue” on first secrets use; cleared when they change PIN in Me → Security. Drives silent startup unlock for default-PIN profiles.

**Stylesheet entry:** RML documents link `foundation.rcss`, `components.rcss`, `colors-light.rcss`, and `colors-dark.rcss`. The legacy `theme` path field remains for compatibility.

See [UI_DESIGN_SYSTEM.md](UI_DESIGN_SYSTEM.md) for tokens and component classes.

## Schema versioning

All JSON stores include `schema_version` (or `config_version` for config). Unsupported newer versions fail with a clear error. Forward migrators can be registered for future v1→v2 changes during development.

**No legacy import:** older flat layouts (e.g. `identity.json` at data root) are not migrated. Delete the data directory when the layout changes during development.

## In-app settings (Me tab)

Open **Me** from the nav rail (person icon). The Me tab shows an **identity card** (nickname, relay ID, Copy ID / Share / Register) above a **Preferences** list → detail layout:

| Section | Persists to | Scope |
|---------|-------------|-------|
| Profile (Me card) | `identity.enc` (+ `vault.bin`) | profile |
| Assistant | `config.json` | machine |
| Integrations | `config.json` | machine |
| Network | `config.json` | machine |
| Appearance | `preferences.json` | profile |
| Security | `vault.bin` + `preferences.json` (`pin_is_default`) | profile |
| Storage | read-only paths | — |

On tab entry, [`SettingsController`](../src/feature/ui/SettingsController.cpp) reloads from disk via `SessionStore::ReloadFromDisk()` so the UI matches persisted files. Changes **auto-save per block**: select fields save immediately; text fields debounce ~500ms. Pending changes flush before switching sections or leaving the tab. Config sections apply through [`SettingsLogic`](../src/feature/settings/SettingsLogic.cpp), write to disk, and reload into `SessionStore`. Config changes hot-reload via listeners → `AgentSession::Configure` and `MessagingHub::Reinitialize`; appearance changes apply via appearance listeners → `Theme::ApplyAppearance`.

### Machine config keys (`config.json`)

```json
{
  "promoted_mcp": { "url": "https://www.brief.global/mcp" },
  "mcp_servers": [
    { "id": "my-tooling", "url": "https://example.com/mcp", "enabled": true }
  ],
  "search": { "provider": "duckduckgo" },
  "relay": { "base_url": "" },
  "directory": { "base_url": "" },
  "registration": { "base_url": "" },
  "libp2p": {
    "listen_multiaddr": "/ip4/0.0.0.0/tcp/40123",
    "max_connections": 48,
    "max_concurrent_dials": 6,
    "dial_timeout_ms": 8000,
    "idle_ttl_ms": 180000,
    "dial_failure_backoff_ms": 30000
  }
}
```

- **`promoted_mcp`** — primary MCP endpoint (feeds, promoted infra tools). Blank URL uses [`PlatformDefaults`](../src/base/platform/PlatformDefaults.cpp).
- **`mcp_servers`** — additional MCP servers (custom tool bucket). Legacy `"mcp"` key loads into `promoted_mcp`.
- **`relay` / `directory` / `registration`** — separate HTTP endpoints. Empty `base_url` falls back to promoted MCP infra tools, then in-process mocks. See [SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md).
- **`libp2p`** — shared host listen address and session policy (on-demand dial, warm-active, idle TTL, connection caps). Default listen is loopback; use a non-loopback multiaddr for LAN/direct peers. Contacts may store dialable `multiaddrs` (must include `/p2p/<PeerId>`).

Enter an **API key** directly in Me → Assistant (saved to `config.json`) or use **API key env var** for desktop-style env lookup. Leaving the password field blank on save keeps an existing saved API key. Default preset is **Cloud**; **Ollama (localhost)** remains available for local dev.

### Verify settings persistence (manual)

```bash
pp-browser --config /tmp/pp-test-config.json
# Me → Assistant → change model → wait briefly → back → reopen Me
jq .llm.model /tmp/pp-test-config.json
```

The on-disk model should match what you set.

## Platform layer

Shared abstractions under `src/base/platform/`:

- `IPathProvider` / `IAssetLocator` — desktop paths vs APK/bundle assets
- `AssetIO` / `SdlAssetFileInterface` — unified bundle reads for UI and RmlUi
- `PlatformDefaults` — cloud LLM on all platforms
- `PlatformNavigation` — Escape and Android back → `ShellHost::HandleDismiss()`
- `AppLifecycle` — background IO pause, agent cancel, P2P poll guard
- `ICredentialStore` / `EnvCredentialStore` — env-backed keys; Keystore deferred

See [PLATFORMS.md](PLATFORMS.md).

## Environment variables

| Variable | Purpose |
|----------|---------|
| `PP_BROWSER_CONFIG` | Explicit config file path |
| `PP_BROWSER_LLM_MODEL` | Default cloud model when no config file |
| `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` | Linux path overrides |

API keys can be set inline in Settings/config (`llm.api_key`) or via `api_key_env` resolved through `ICredentialStore`.
