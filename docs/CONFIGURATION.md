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
  identity.json
  contacts.json
  threads/...
```

Phase 1 ships a single `default` profile. Use `--profile NAME` for dev isolation (no account-switcher UI yet).

## Config resolution

1. `--config PATH`
2. `PP_BROWSER_CONFIG` environment variable
3. `{config_dir}/config.json`

There is **no** CWD `config.json` discovery. For local dev: `pp-browser --config config.json.example`.

Layering: `PlatformDefaults` → user config file → field-level merge (partial JSON is valid). Serialization lives in `src/base/data/ConfigJson.*` (nlohmann `to_json` / `from_json` with deep merge).

## Runtime session state

After bootstrap, a single [`SessionStore`](../src/base/data/SessionStore.h) owns the live `BootstrapResult` (config, profile prefs, paths). Settings and demos read/write through it; saves reload from disk before notifying listeners.

| Listener | Trigger |
|----------|---------|
| Config | `SessionStore::SaveConfig` → `ChatDemo` / `AgentSession::Configure` |
| Theme | `SessionStore::SaveProfilePrefs` → `Theme::LoadBase` |

## LLM presets

`config.json` may include `llm.preset`: `"cloud"`, `"ollama"`, or `"custom"`. Preset metadata and apply logic live in `src/base/data/LlmPreset.*`. Legacy files without `preset` infer it once from `base_url`.

## Theme and appearance

**Appearance (light/dark):** `profiles/{id}/preferences.json` → `appearance` (`system`, `light`, or `dark`). System follows `SDL_GetSystemTheme` and live-updates on `SDL_EVENT_SYSTEM_THEME_CHANGED`.

**Stylesheet entry:** RML documents link `foundation.rcss`, `components.rcss`, `colors-light.rcss`, and `colors-dark.rcss`. The legacy `theme` path field remains for compatibility.

See [UI_DESIGN_SYSTEM.md](UI_DESIGN_SYSTEM.md) for tokens and component classes.

## Schema versioning

All JSON stores include `schema_version` (or `config_version` for config). Unsupported newer versions fail with a clear error. Forward migrators can be registered for future v1→v2 changes during development.

**No legacy import:** older flat layouts (e.g. `identity.json` at data root) are not migrated. Delete the data directory when the layout changes during development.

## In-app settings

Open **Settings** from the sidebar footer. Saves machine config to `config.json` and theme to profile `preferences.json`. LLM changes hot-reload via `SessionStore` config listeners → `AgentSession::Configure`.

While Settings is open, [`SettingsController`](../src/feature/ui/SettingsController.cpp) keeps a **draft buffer** (`draft_`) separate from the live `SessionStore` snapshot. Edits update the draft via `data-value` bindings and explicit `data-event-change` handlers (model, base URL, theme, API key env). **Save** applies the draft through [`ApplySettingsDraft`](../src/feature/settings/SettingsLogic.cpp) (including LLM preset defaults) and persists via `SessionStore`. Closing Settings without saving discards the draft.

Enter an **API key** directly in Settings (saved to `config.json`) or use **API key env var** for desktop-style env lookup. Leaving the password field blank on save keeps an existing saved API key. Default preset is **Cloud**; **Ollama (localhost)** remains available for local dev.

### Verify settings persistence (manual)

```bash
pp-browser --config /tmp/pp-test-config.json
# Settings → change LLM model → Save → back → reopen Settings
jq .llm.model /tmp/pp-test-config.json
```

The on-disk model should match what you saved.

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
