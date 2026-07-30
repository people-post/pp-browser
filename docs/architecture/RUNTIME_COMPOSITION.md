# Runtime composition

**Tier:** architecture  
**Related:** [ARCHITECTURE.md](ARCHITECTURE.md) (system overview), [SRC_LAYOUT.md](SRC_LAYOUT.md) (layers / includes), [ops/CONFIGURATION.md](../ops/CONFIGURATION.md) (disk DTOs → service slices).

How **Application** relates to the main service modules at runtime: ownership, feature-link order, and settings hot-reload.

## Layers (link / include direction)

```
app → feature → base → common
```

Feature libraries link acyclically: `settings → ai → messaging → ui → chat`. Cross-controller wiring and SessionStore fan-out live in `src/app/` ([`ConfigApplyBridge`](../../src/app/ConfigApplyBridge.h)).

```mermaid
flowchart TB
  subgraph app_layer["app/"]
    Application
    ConfigApplyBridge
    Bootstrap
  end

  subgraph feature_layer["feature/"]
    settings["settings/"]
    ai["ai/ + tools + bindings"]
    messaging["messaging/ MessagingHub"]
    ui["ui/ ShellHost · Settings · Contacts · PinGate"]
    chat["chat/ ChatController"]
  end

  subgraph base_layer["base/"]
    data["data/ SessionStore · AppConfig · ProfilePreferences"]
    i18n["i18n/ LocalizationService"]
    base_ui["ui/ Theme"]
    people["people/ Identity · Contacts"]
    base_msg["messaging/ stores · codecs"]
    base_ai["ai/ LlmClient · Conversation"]
    media["media/"]
  end

  subgraph sidecars["fork sidecars"]
    rmlui["render/ RmlUi + SDL/GL"]
    libp2p["libp2p/"]
  end

  Application --> ConfigApplyBridge
  Application --> chat
  Application --> ui
  Application --> messaging
  ConfigApplyBridge --> messaging
  ConfigApplyBridge --> ui
  ConfigApplyBridge --> i18n
  ConfigApplyBridge --> base_ui
  ConfigApplyBridge --> data

  chat --> ui
  chat --> messaging
  chat --> ai
  ui --> messaging
  ui --> settings
  messaging --> ai
  messaging --> libp2p
  messaging --> people
  messaging --> base_msg
  messaging --> media
  ui --> rmlui
  chat --> rmlui
  settings --> data
  ai --> base_ai
  i18n --> data
```

## Runtime composition (who owns whom)

Solid arrows = ownership / wiring from the composition root. Dashed = coordination bridges (lifecycle or command hooks), not data ownership.

```mermaid
flowchart LR
  subgraph composition["Composition root"]
    App["Application"]
    Bridge["ConfigApplyBridge"]
    Store["SessionStore<br/>AppConfig + ProfilePreferences"]
  end

  subgraph surfaces["UI surfaces"]
    Shell["ShellHost"]
    Settings["SettingsController"]
    Chat["ChatController"]
    Contacts["ContactsController"]
    Pin["PinGateController"]
  end

  subgraph services["Core services"]
    Hub["MessagingHub"]
    Agent["AgentSession"]
    Locale["LocalizationService"]
    Theme["Theme"]
  end

  App --> Bridge
  App --> Hub
  App --> Shell
  App --> Chat
  App --> Settings
  App --> Contacts
  App --> Pin

  Bridge --> Store
  Bridge -->|Project + Apply| Hub
  Bridge -->|ChromePrefs| Shell
  Bridge -->|Prefs| Locale
  Bridge -->|theme/appearance| Theme

  Settings -->|flush disk DTOs only| Store
  Chat -->|AddConfigListener LLM| Store
  Chat --> Hub
  Chat --> Agent
  Hub --> Agent

  Shell -.->|layout / Me sheet lifecycle| Settings
  Chat -.->|ChatSessionActions| Settings
  Pin -.->|unlock gate| Settings
  Contacts -.->|hub-bound| Hub
```

## Settings / prefs hot-reload

Disk DTOs stay in `SessionStore`. Services expose **nested** slice types (`MessagingHub::NetworkConfig`, `ShellHost::ChromePrefs`, …). [`ConfigApplyBridge`](../../src/app/ConfigApplyBridge.h) projects and applies only when a slice changes. Field-level howto: [CONFIGURATION.md](../ops/CONFIGURATION.md).

```mermaid
flowchart TB
  UI["Settings UI / section Flush"]
  Disk["Disk DTOs<br/>AppConfig · ProfilePreferences"]
  Store["SessionStore notify"]
  Bridge["ConfigApplyBridge<br/>Project* + last-slice gate"]

  UI -->|SaveConfig / SaveProfilePrefs| Disk
  Disk --> Store
  Store --> Bridge

  Bridge --> N["MessagingHub::NetworkConfig"]
  Bridge --> P["MessagingHub::PolicyPrefs"]
  Bridge --> Push["MessagingHub::NotificationPrefs"]
  Bridge --> C["ShellHost::ChromePrefs"]
  Bridge --> L["LocalizationService::Prefs"]
  Store --> AgentCfg["ChatController<br/>full AppConfig still"]

  N --> HubA["MessagingHub::Apply"]
  P --> HubA
  Push --> HubA
  C --> ShellA["Theme + ShellHost::Apply"]
  L --> LocA["LocalizationService::Apply"]
```

## Notable modules

| Module | Role |
|--------|------|
| **Application** | Owns hub lifetime, binds controllers, installs `ConfigApplyBridge` |
| **SessionStore** | Live disk DTOs; notifies on save/reload |
| **ConfigApplyBridge** | Projects nested service slices; fans out `Apply` |
| **MessagingHub** | P2P / inbox / identity / mesh; nested `NetworkConfig` / `PolicyPrefs` / `NotificationPrefs` |
| **ShellHost** | Window shell panes/nav; nested `ChromePrefs` |
| **LocalizationService** | Locale catalogs; nested `Prefs` |
| **SettingsController** | Me-tab UI + flush to SessionStore (not service apply) |
| **ChatController** | Chat UI + agent; still listens to full `AppConfig` for LLM |
| **AgentSession** | Turn plan/execute; bound from hub/chat |
