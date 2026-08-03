# Runtime composition

**Tier:** architecture  
**Related:** [ARCHITECTURE.md](ARCHITECTURE.md) (system overview), [SRC_LAYOUT.md](SRC_LAYOUT.md) (layers / includes), [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md) (UI vs functional contracts), [ops/CONFIGURATION.md](../ops/CONFIGURATION.md) (disk DTOs → service slices).

How **Application** relates to the main service modules at runtime: ownership, feature-link order, settings hot-reload, and **threading** (UI / IO / libp2p / media).

For **what UI may call** (state / config / actions / events, facades vs ports), see [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md). UI ↔ functional decoupling is **complete** (Phase 8, 2026-08): all presenters and `ShellHost` are app-owned; cross-presenter calls use notify ports or `Application` wiring.

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
    PeoplePicker["PeoplePickerController<br/><small>feature/ui/</small>"]
    Pin["PinGateController<br/><small>feature/ui/ — presentation</small>"]
    UnlockGate["ProfileUnlockGate<br/><small>base/crypto/</small>"]
  end

  subgraph services["Core services"]
    Hub["MessagingHub<br/><small>feature/messaging/</small>"]
    Agent["AgentSession<br/><small>feature/ai/</small>"]
    Locale["LocalizationService<br/><small>base/i18n/</small>"]
    ThemeNode["Theme<br/><small>base/ui/</small>"]
    ActionRouter["ActionRouter<br/><small>feature/ai/bindings/</small>"]
    ClientCompat["ClientCompatController<br/><small>feature/ui/</small>"]
    Badges["BadgeAggregator<br/><small>feature/ui/</small>"]
    Input["InputCoordinator<br/><small>base/ui/</small>"]
    Flow["FlowCoordinator<br/><small>feature/ui/</small>"]
    Call["CallController<br/><small>feature/ui/</small>"]
  end

  App --> Bridge
  App --> Hub
  App --> Shell
  App --> Chat
  App --> Settings
  App --> Contacts
  App --> PeoplePicker
  App --> UnlockGate
  App --> Pin
  App --> ActionRouter
  App --> ClientCompat
  App --> Badges
  App --> Input
  App --> Flow
  App --> Call

  Bridge --> Store
  Bridge -->|Project + Apply| Hub
  Bridge -->|ChromePrefs| Shell
  Bridge -->|AgentConfig| Chat
  Bridge -->|Prefs| Locale
  Bridge -->|theme / appearance| ThemeNode

  Settings -->|flush disk DTOs only| Store
  Chat -->|AddConfigListener LLM| Store
  Chat -->|MessagingChatPorts| Hub
  App --> Agent
  App -->|BindAgentPorts| Chat
  Chat -->|BindBadgeAggregator| Badges
  Chat -->|BindInputCoordinator| Input
  Chat -->|BindCallController| Call
  App -->|BindCallController| Shell
  App -->|BindSource| Badges
  App -->|BindPorts| UnlockGate
  UnlockGate -->|UI ports| Pin

  App -->|WireShellPresentationEvents| Shell
  App -->|BindCommands SettingsCommands| Settings
  App -->|BindChatPorts / ContactsNotifyPorts| Contacts
  App -->|BindChatPorts| PeoplePicker
  App -->|BindPeoplePickerNotify| Chat
  App -->|BindPeoplePickerNotify| Call
  App -->|BindPinGate| Shell
  App -->|BindFlowCoordinator| Shell
  App -->|BindFlowCoordinator| PeoplePicker
  App -->|BindUnlockGate| Chat
  App -->|BindUnlockGate| Settings
  App -->|BindUnlockGate| Contacts
  App -->|BindUnlockGate| PeoplePicker
  App -->|deferred startup| ClientCompat
  App -->|deferred startup| UnlockGate
  UnlockGate -.->|unlock gate| Settings
  Contacts -->|MessagingContactsPorts| Hub
  PeoplePicker -->|MessagingContactsPorts / MessagingPeoplePickerPorts| Hub
  Shell -->|MessagingShellPorts| Hub
  Call -->|MessagingCallPorts| Hub
  Settings -->|ShellNavigationPorts / ShellFeedbackPorts| Shell
  Chat -->|ShellNavigationPorts / ShellFeedbackPorts| Shell
  Contacts -->|ShellNavigationPorts / ShellFeedbackPorts| Shell
  PeoplePicker -->|ShellNavigationPorts / ShellFeedbackPorts| Shell
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
  Bridge --> A["ChatController::AgentConfig<br/><small>from AppConfig</small>"]

  N --> HubA["MessagingHub::Apply"]
  P --> HubA
  Push --> HubA
  C --> ShellA["Theme + ShellHost::Apply"]
  L --> LocA["LocalizationService::Apply"]
  A --> ChatA["ChatController::Apply"]
```

## Allowed edges (hot-reload / settings)

| From | To | Allowed? |
|------|-----|----------|
| Settings section flush | `SessionStore` SaveConfig / SaveProfilePrefs | Yes |
| Settings UI | `MessagingHub::Apply*` / mesh / invite policy | **No** — go through SessionStore → bridge |
| Settings UI | register / rotate / UPnP / clear undelivered / reset profile / appearance / locales / reachability / PIN status | Via `SettingsCommands` (narrow args + views; app-filled); UI syncs state after — **no** `BindMessaging` / `Hub()` |
| `ConfigApplyBridge` | nested `Apply` on services | Yes |
| ChatController | full `AppConfig` listener | **No** — agent slice via bridge |
| ChatController | `SetOnMessagingReady` / reachability | **No** — Application owns |
| Application Run loop | `MessagingHub::TickLibp2p` | Yes |
| UI presenter | Another controller `::Instance()` | **No** — coordinator or ports ([UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md)) |
| Functional system | `ShellHost::State()` mutation | **No** — UI ports / events |

## Threading

pp-browser uses a small set of **owned** threads plus short-lived **hop-off** workers. UI work is sequenced on the main thread; blocking network/LLM work goes to `BrowserThread::IO`. libp2p and call media run their own loops.

```mermaid
flowchart TB
  subgraph main["Main / UI thread"]
    SDL["Application::Run<br/><small>SDL event loop</small>"]
    UIQ["BrowserThread::UI<br/><small>SequencedTaskRunner — drain via RunUITasks</small>"]
    ShellTick["ShellHost · ChatController<br/><small>feature/ui · feature/chat</small>"]
    TickLp["MessagingHub::TickLibp2p<br/><small>app Run loop when messaging ready</small>"]
    SDL --> UIQ
    SDL --> ShellTick
    SDL --> TickLp
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
- **`MessagingHub::TickLibp2p`** runs from `Application::Run` when messaging is ready — not from ChatController.
- Pause/resume: `AgentSession` may `BrowserThread::PauseIO` / `ResumeIO` around sensitive UI transitions.

## Notable modules

| Class | Location | Role |
|-------|----------|------|
| **Application** | `app/` | Owns hub, shell, all presenters (`SettingsController`, `ContactsController`, `PeoplePickerController`, `ChatController`, `ShellHost`), `AgentSession`, ActionRouter / ClientCompat / BadgeAggregator / InputCoordinator / FlowCoordinator / CallController / ProfileUnlockGate / PinGate UI; binds ports; installs `ConfigApplyBridge` |
| **SessionStore** | `base/data/` | Live disk DTOs; notifies on save/reload |
| **ConfigApplyBridge** | `app/` | Projects nested service slices; fans out `Apply` |
| **MessagingHub** | `feature/messaging/` | P2P / inbox / identity / mesh; `LoadProfileIdentityView`, register, rotate; nested network/policy slices |
| **ActionRouter** | `feature/ai/bindings/` | Rml action → tool routing; app-owned |
| **ClientCompatController** | `feature/ui/` | Relay client-compat check; app-owned; deferred startup |
| **BadgeAggregator** | `feature/ui/` | Nav unread badges; app-owned; `BindSource` from MessagingHub; chat calls Refresh |
| **InputCoordinator** | `base/ui/` | Key bindings; app-owned; chat registers Enter-to-send |
| **FlowCoordinator** | `feature/ui/` | Modal overlay dismiss/step-back; app-owned; Shell + PeoplePicker |
| **CallController** | `feature/ui/` | Call ring / in-call chrome; app-owned; Shell binds for Rml chrome; chat starts/wakes |
| **PinGateController** | `feature/ui/` | PIN overlay presentation; UI ports for ProfileUnlockGate |
| **ProfileUnlockGate** | `base/crypto/` | Vault unlock policy + caller queue; messaging/UI via ports |
| **ShellHost** | `feature/ui/` | Window shell panes/nav; nested `ChromePrefs` |
| **LocalizationService** | `base/i18n/` | Locale catalogs; nested `Prefs` |
| **SettingsController** | `feature/ui/` | Me-tab UI + flush via `session_store` port; holds injected `SettingsCommands` only (no messaging bind) |
| **SettingsCommands** | `feature/settings/` | Ports for session, identity, locale, appearance, reachability, PIN status, imperative ops; app binds implementations |
| **ChatSessionPorts** | `feature/ui/` | Chat nav ports for contacts/people-picker; app-filled from `ChatController` |
| **ContactsNotifyPorts** | `feature/ui/` | Contacts refresh/select for chat; app-filled from `ContactsController` |
| **PeoplePickerNotifyPorts** | `feature/ui/` | Open-picker hooks for chat/call; app-filled from `PeoplePickerController` |
| **ProfileIdentityView** | `base/people/` | Presentation projection of local identity |
| **ChatController** | `feature/chat/` | Chat UI + agent; nested `AgentConfig` |
| **AgentSession** | `feature/ai/` | Turn plan/execute; bound from hub/chat |
| **BrowserThread** | `base/platform/` | UI + IO sequenced runners |
| **Libp2pHost** | `libp2p/integration/host/` | Vendored host + asio IO thread |
| **CallMediaEngine** | `base/media/` | A/V capture threads over libdatachannel |