# Runtime composition

**Tier:** architecture  
**Related:** [ARCHITECTURE.md](ARCHITECTURE.md) (system overview), [SRC_LAYOUT.md](SRC_LAYOUT.md) (layers / includes), [ops/CONFIGURATION.md](../ops/CONFIGURATION.md) (disk DTOs → service slices).

How **Application** relates to the main service modules at runtime: ownership, feature-link order, settings hot-reload, and **threading** (UI / IO / libp2p / media).

## Layers (link / include direction)

```
app → feature → base → common
```

Feature libraries link acyclically: `settings → ai → messaging → ui → chat`. Cross-controller wiring and SessionStore fan-out live in `src/app/` ([`ConfigApplyBridge`](../../src/app/ConfigApplyBridge.h)).

```mermaid
flowchart TB
  subgraph app_layer["app/"]
    Application["Application<br/><small>app/Application</small>"]
    ConfigApplyBridge["ConfigApplyBridge<br/><small>app/ConfigApplyBridge</small>"]
    Bootstrap["Bootstrap<br/><small>app/Bootstrap</small>"]
  end

  subgraph feature_layer["feature/"]
    SettingsLogic["SettingsLogic<br/><small>feature/settings/</small>"]
    AgentSession["AgentSession<br/><small>feature/ai/</small>"]
    MessagingHub["MessagingHub<br/><small>feature/messaging/</small>"]
    ShellHost["ShellHost<br/><small>feature/ui/</small>"]
    SettingsController["SettingsController<br/><small>feature/ui/</small>"]
    ChatController["ChatController<br/><small>feature/chat/</small>"]
  end

  subgraph base_layer["base/"]
    SessionStore["SessionStore<br/><small>base/data/</small>"]
    LocalizationService["LocalizationService<br/><small>base/i18n/</small>"]
    Theme["Theme<br/><small>base/ui/</small>"]
    IdentityStore["IdentityStore<br/><small>base/people/</small>"]
    ThreadStore["SqliteThreadStore<br/><small>base/messaging/</small>"]
    LlmClient["LlmClient<br/><small>base/ai/</small>"]
    CallMediaEngine["CallMediaEngine<br/><small>base/media/</small>"]
  end

  subgraph sidecars["fork sidecars"]
    RmlUi["RmlUi Context<br/><small>render/fork + integration</small>"]
    Libp2p["Libp2pHost<br/><small>libp2p/fork + integration</small>"]
  end

  Application --> ConfigApplyBridge
  Application --> ChatController
  Application --> ShellHost
  Application --> MessagingHub
  ConfigApplyBridge --> MessagingHub
  ConfigApplyBridge --> ShellHost
  ConfigApplyBridge --> LocalizationService
  ConfigApplyBridge --> Theme
  ConfigApplyBridge --> SessionStore

  ChatController --> ShellHost
  ChatController --> MessagingHub
  ChatController --> AgentSession
  ShellHost --> MessagingHub
  SettingsController --> SettingsLogic
  MessagingHub --> AgentSession
  MessagingHub --> Libp2p
  MessagingHub --> IdentityStore
  MessagingHub --> ThreadStore
  MessagingHub --> CallMediaEngine
  ShellHost --> RmlUi
  ChatController --> RmlUi
  SettingsLogic --> SessionStore
  AgentSession --> LlmClient
  LocalizationService --> SessionStore
```

## Runtime composition (who owns whom)

Solid arrows = ownership / wiring from the composition root. Dashed = coordination bridges (lifecycle or command hooks), not data ownership.

```mermaid
flowchart LR
  subgraph composition["Composition root"]
    App["Application<br/><small>app/</small>"]
    Bridge["ConfigApplyBridge<br/><small>app/</small>"]
    Store["SessionStore<br/><small>base/data/</small>"]
  end

  subgraph surfaces["UI surfaces"]
    Shell["ShellHost<br/><small>feature/ui/</small>"]
    Settings["SettingsController<br/><small>feature/ui/</small>"]
    Chat["ChatController<br/><small>feature/chat/</small>"]
    Contacts["ContactsController<br/><small>feature/ui/</small>"]
    Pin["PinGateController<br/><small>feature/ui/</small>"]
  end

  subgraph services["Core services"]
    Hub["MessagingHub<br/><small>feature/messaging/</small>"]
    Agent["AgentSession<br/><small>feature/ai/</small>"]
    Locale["LocalizationService<br/><small>base/i18n/</small>"]
    ThemeNode["Theme<br/><small>base/ui/</small>"]
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
  Bridge -->|theme / appearance| ThemeNode

  Settings -->|flush disk DTOs only| Store
  Chat -->|AddConfigListener LLM| Store
  Chat --> Hub
  Chat --> Agent
  Hub --> Agent

  Shell -.->|layout / Me sheet| Settings
  Chat -.->|ChatSessionActions| Settings
  Pin -.->|unlock gate| Settings
  Contacts -.->|hub-bound| Hub
```

## Settings / prefs hot-reload

Disk DTOs stay in `SessionStore`. Services expose **nested** slice types (`MessagingHub::NetworkConfig`, `ShellHost::ChromePrefs`, …). [`ConfigApplyBridge`](../../src/app/ConfigApplyBridge.h) projects and applies only when a slice changes. Field-level howto: [CONFIGURATION.md](../ops/CONFIGURATION.md).

```mermaid
flowchart TB
  UI["SettingsController<br/><small>feature/ui/ — section Flush</small>"]
  Disk["AppConfig · ProfilePreferences<br/><small>disk DTOs via SessionStore</small>"]
  Store["SessionStore<br/><small>base/data/ — notify</small>"]
  Bridge["ConfigApplyBridge<br/><small>app/ — Project* + last-slice gate</small>"]

  UI -->|SaveConfig / SaveProfilePrefs| Disk
  Disk --> Store
  Store --> Bridge

  Bridge --> N["MessagingHub::NetworkConfig<br/><small>from AppConfig</small>"]
  Bridge --> P["MessagingHub::PolicyPrefs<br/><small>from ProfilePreferences</small>"]
  Bridge --> Push["MessagingHub::NotificationPrefs<br/><small>from ProfilePreferences</small>"]
  Bridge --> C["ShellHost::ChromePrefs<br/><small>from ProfilePreferences</small>"]
  Bridge --> L["LocalizationService::Prefs<br/><small>from ProfilePreferences</small>"]
  Store --> AgentCfg["ChatController::ApplyRuntimeConfig<br/><small>full AppConfig still</small>"]

  N --> HubA["MessagingHub::Apply"]
  P --> HubA
  Push --> HubA
  C --> ShellA["Theme + ShellHost::Apply"]
  L --> LocA["LocalizationService::Apply"]
```

## Threading

pp-browser uses a small set of **owned** threads plus short-lived **hop-off** workers. UI work is sequenced on the main thread; blocking network/LLM work goes to `BrowserThread::IO`. libp2p and call media run their own loops.

```mermaid
flowchart TB
  subgraph main["Main / UI thread"]
    SDL["Application::Run<br/><small>SDL event loop</small>"]
    UIQ["BrowserThread::UI<br/><small>SequencedTaskRunner — drain via RunUITasks</small>"]
    ShellTick["ShellHost · ChatController<br/><small>feature/ui · feature/chat</small>"]
    TickLp["MessagingHub::TickLibp2p<br/><small>session policy on UI tick</small>"]
    SDL --> UIQ
    SDL --> ShellTick
    ShellTick --> TickLp
  end

  subgraph app_io["App IO thread"]
    IOQ["BrowserThread::IO<br/><small>SequencedTaskRunner dedicated thread</small>"]
    Http["HttpClient<br/><small>base/net — sync libcurl on caller</small>"]
    AgentIO["AgentSession<br/><small>feature/ai — LLM / tools on IO</small>"]
    P2pIO["P2pMessagingService<br/><small>feature/messaging — relay poll / sync / send</small>"]
    IOQ --> Http
    IOQ --> AgentIO
    IOQ --> P2pIO
  end

  subgraph libp2p_stack["libp2p host"]
    LpIo["Libp2pHost io_thread_<br/><small>boost::asio::io_context::run</small>"]
    Host["libp2p::Host<br/><small>libp2p/fork — Yamux + Noise</small>"]
    Hop["Protocol hop-off threads<br/><small>DialBack · CircuitRelay · MediaRelay · Reachability</small>"]
    LpIo --> Host
    Host -.->|handlers must not block| Hop
  end

  subgraph media_stack["Call media"]
    Cap["CallMediaEngine capture_thread<br/><small>base/media — mic / encode</small>"]
    Vid["CallMediaEngine video_thread<br/><small>base/media — camera / encode</small>"]
    Ring["CallRingtone thread_<br/><small>base/media</small>"]
    RtcPool["libdatachannel ThreadPool<br/><small>third_party — ~hardware_concurrency</small>"]
    Cap --> RtcPool
    Vid --> RtcPool
  end

  subgraph platform_extra["Platform extras"]
    Notif["ILocalNotifier watch thread<br/><small>Linux D-Bus Freedesktop; join on Shutdown</small>"]
  end

  UIQ -->|"PostTask(UI)"| UIQ
  IOQ -->|"PostTask(UI) replies"| UIQ
  AgentIO -->|"PostTask(UI) events"| UIQ
  P2pIO -->|"PostTask(UI) inbox / notices"| UIQ
  Hop -->|"optional UI refresh"| UIQ
  TickLp -->|"PeerSessionManager on UI"| Host
  P2pIO -.->|"async dial / streams"| LpIo
  Cap -.->|"WebRTC"| RtcPool
```

### Thread inventory

| Thread / queue | Owner class | Location | Role |
|----------------|-------------|----------|------|
| **Main / UI** | `Application` + `BrowserThread::UI` | `app/` · `base/platform/` | SDL loop, RmlUi, shell/chat; UI runner has **no** dedicated thread — drained by `RunUITasks()` |
| **App IO** | `BrowserThread::IO` | `base/platform/` · `common/SequencedTaskRunner` | Dedicated thread: HTTP (libcurl), LLM/tool work, relay poll/sync/send |
| **libp2p IO** | `Libp2pHost` | `libp2p/integration/host/` | `boost::asio::io_context` run loop for the vendored host |
| **Protocol hop-offs** | `DialBackService`, `CircuitRelayService`, `MediaRelayService`, `ReachabilityService` | `libp2p/integration/host/` | Short-lived `std::thread`s so protocol handlers do not block the host IO thread |
| **Media capture / video** | `CallMediaEngine` | `base/media/` | Dedicated capture + video encode loops |
| **Ringtone** | `CallRingtone` | `base/media/` | Playback loop thread |
| **WebRTC pool** | libdatachannel `ThreadPool` (+ optional poll/ICE loops) | `third_party/libdatachannel/` | Vendored pool sized from `hardware_concurrency` |
| **Notification watch** | `ILocalNotifier` (Linux) | `base/platform/desktop/` | D-Bus ActionInvoked watcher; joined in `Shutdown` |

### Cross-thread rules of thumb

- **UI** owns RmlUi, shell state, and most controller mutations. Prefer `BrowserThread::PostTask(UI, …)` from IO / workers.
- **IO** owns blocking Brief HTTP (`HttpClient` / libcurl) and agent network work. `PostTaskAndReply` is IO → UI.
- **libp2p IO** must stay non-blocking for dials/reads; integration services hop to detached workers, then post results as needed.
- **`MessagingHub::TickLibp2p`** runs on the UI tick (from `ChatController`) for idle/session policy — not on the asio thread.
- Pause/resume: `AgentSession` may `BrowserThread::PauseIO` / `ResumeIO` around sensitive UI transitions.

## Notable modules

| Class | Location | Role |
|-------|----------|------|
| **Application** | `app/` | Owns hub lifetime, binds controllers, installs `ConfigApplyBridge` |
| **SessionStore** | `base/data/` | Live disk DTOs; notifies on save/reload |
| **ConfigApplyBridge** | `app/` | Projects nested service slices; fans out `Apply` |
| **MessagingHub** | `feature/messaging/` | P2P / inbox / identity / mesh; nested `NetworkConfig` / `PolicyPrefs` / `NotificationPrefs` |
| **ShellHost** | `feature/ui/` | Window shell panes/nav; nested `ChromePrefs` |
| **LocalizationService** | `base/i18n/` | Locale catalogs; nested `Prefs` |
| **SettingsController** | `feature/ui/` | Me-tab UI + flush to SessionStore (not service apply) |
| **ChatController** | `feature/chat/` | Chat UI + agent; still listens to full `AppConfig` for LLM |
| **AgentSession** | `feature/ai/` | Turn plan/execute; bound from hub/chat |
| **BrowserThread** | `base/platform/` | UI + IO sequenced runners |
| **Libp2pHost** | `libp2p/integration/host/` | Vendored host + asio IO thread |
| **CallMediaEngine** | `base/media/` | A/V capture threads over libdatachannel |