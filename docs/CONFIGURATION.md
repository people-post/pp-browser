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

Layering: `PlatformDefaults` → user config file → field-level merge (partial JSON is valid).

## Schema versioning

All JSON stores include `schema_version` (or `config_version` for config). Unsupported newer versions fail with a clear error. Forward migrators can be registered for future v1→v2 changes during development.

**No legacy import:** older flat layouts (e.g. `identity.json` at data root) are not migrated. Delete the data directory when the layout changes during development.

## In-app settings

Open **Settings** from the sidebar footer. Saves to user config dir and profile preferences. LLM changes hot-reload via `AgentSession::Configure`.

## Mobile readiness (not shipped)

Platform seams live under `src/platform/`:

- `IPathProvider` / `DesktopPathProvider` — swap for sandbox paths on Android/iOS
- `IAssetLocator` / `DesktopAssetLocator` — bundle assets on mobile
- `PlatformDefaults` — desktop Ollama vs future cloud LLM defaults
- `ICredentialStore` / `EnvCredentialStore` — future keychain/keystore
- `machine.json` `safe_area` stub for notch/home-indicator padding

Future `PlatformNavigation::OnSystemBack()` should map to `ShellHost::HandleDismiss()` (desktop keeps Escape).

See [PLATFORMS.md](PLATFORMS.md).

## Environment variables

| Variable | Purpose |
|----------|---------|
| `PP_BROWSER_CONFIG` | Explicit config file path |
| `PP_BROWSER_LLM_MODEL` | Default Ollama model when no config file |
| `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` | Linux path overrides |

API keys can use `api_key_env` in config; resolved via `ICredentialStore` (env-backed on desktop).
