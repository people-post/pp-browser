# Runtime composition

**Tier:** architecture  
**Related:** [ARCHITECTURE.md](ARCHITECTURE.md) (system overview), [SRC_LAYOUT.md](SRC_LAYOUT.md) (layers / includes), [OWNERSHIP.md](OWNERSHIP.md) (parent-only destroy), [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md) (UI vs functional contracts), [ops/CONFIGURATION.md](../ops/CONFIGURATION.md) (disk DTOs → service slices).

How **Application** relates to the main service modules at runtime: ownership, feature-link order, settings hot-reload, and **threading** (UI / IO / libp2p / media). Lifetime edges follow [OWNERSHIP.md](OWNERSHIP.md) (composition root owns services; children request stop).

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
    CallStack["CallStack<br/><small>feature/messaging/ — call media/CSM/lifecycle</small>"]
    MessagingFacade["MessagingFacade<br/><small>feature/messaging/</small>"]
    ShellHost["ShellHost<br/><small>feature/ui/</small>"]
    SettingsController["SettingsController<br/><small>feature/ui/</small>"]
    ChatController["ChatController<br/><small>gui/chat/</small>"]
  end

  subgraph base_layer["base/"]
    SessionStore["SessionStore<br/><small>base/data/</small>"]
    LocalizationService["LocalizationService<br/><small>base/i18n/</small>"]
    Theme["Theme<br/><small>domain/ui/</small>"]
    IdentityStore["IdentityStore<br/><small>base/people/</small>"]
    ThreadStore["SqliteThreadStore<br/><small>domain/messaging/</small>"]
    LlmClient["LlmClient<br/><small>domain/ai/</small>"]
    CallMediaEngine["CallMediaEngine<br/><small>base/media/</small>"]
  end

  subgraph libs["lib + base glue"]
    RmlUi["RmlUi Context<br/><small>lib/rmlui + base/render</small>"]
    Libp2p["Libp2pHost<br/><small>lib/libp2p + base/p2p</small>"]
  end

  Application --> ConfigApplyBridge
  Application --> ChatController
  Application --> ShellHost
  Application --> MessagingHub
  Application -->|owns MessagingFacade| MessagingFacade
  MessagingFacade --> MessagingHub
  ConfigApplyBridge --> MessagingHub
  ConfigApplyBridge --> ShellHost
  ConfigApplyBridge --> LocalizationService
  ConfigApplyBridge --> Theme
  ConfigApplyBridge --> SessionStore

  ChatController --> ShellHost
  ChatController --> MessagingFacade
  ChatController --> AgentSession
  ShellHost --> MessagingHub
  SettingsController --> SettingsLogic
  MessagingHub --> AgentSession
  MessagingHub --> Libp2p
  MessagingHub --> IdentityStore
  MessagingHub --> ThreadStore
  MessagingHub -->|owns unique_ptr, forwards Calls/Lifecycle| CallStack
  CallStack --> CallMediaEngine
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
    Chat["ChatController<br/><small>gui/chat/</small>"]
    Contacts["ContactsController<br/><small>feature/ui/</small>"]
    PeoplePicker["PeoplePickerController<br/><small>feature/ui/</small>"]
    Pin["PinGateController<br/><small>gui/ — presentation</small>"]
    UnlockGate["ProfileUnlockGate<br/><small>base/crypto/</small>"]
  end

  subgraph services["Core services"]
    Hub["MessagingHub<br/><small>feature/messaging/</small>"]
    Mesh["MeshHost<br/><small>domain/mesh/ — shared w/ pp-node</small>"]
    Agent["AgentSession<br/><small>feature/ai/</small>"]
    Locale["LocalizationService<br/><small>base/i18n/</small>"]
    ThemeNode["Theme<br/><small>domain/ui/</small>"]
    ActionRouter["ActionRouter<br/><small>feature/ai/bindings/</small>"]
    ClientCompat["ClientCompatController<br/><small>feature/ui/</small>"]
    Badges["BadgeAggregator<br/><small>feature/ui/</small>"]
    Input["InputCoordinator<br/><small>domain/ui/</small>"]
    Flow["FlowCoordinator<br/><small>feature/ui/</small>"]
    Call["CallController<br/><small>feature/ui/</small>"]
  end

  App --> Bridge
  App --> Hub
  Hub -->|owns MeshHost| Mesh
  Mesh -.->|same start path| Node["pp-node<br/><small>app/node/NodeBootstrap</small>"]
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
  Chat -->|MessagingFacade| Hub
  App --> Agent
  App -->|BindAgentPorts| Chat
  Chat -->|BindInputCoordinator| Input
  App -->|BindBadgeNotify BadgeNotifyPorts| Chat
  App -->|BindCallActions CallActionsPorts| Chat
  App -->|BindCallActions CallActionsPorts| Shell
  App -->|BindCallActions CallActionsPorts| PeoplePicker
  App -->|BindSource| Badges
  App -->|BindPorts| UnlockGate
  UnlockGate -->|UI ports| Pin

  App -->|WireShellPresentationEvents| Shell
  App -->|BindCommands SettingsCommands| Settings
  App -->|BindChatPorts / ContactsNotifyPorts| Contacts
  App -->|BindChatPorts| PeoplePicker
  App -->|BindPeoplePickerNotify| Chat
  App -->|BindPeoplePickerNotify| Call
  App -->|BindPinGateActions PinGateActionPorts| Shell
  App -->|BindFlowCoordinator FlowCoordinatorPorts| Shell
  App -->|BindFlowCoordinator FlowCoordinatorPorts| PeoplePicker
  App -->|BindUnlockEnsure UnlockEnsurePorts| Chat
  App -->|BindUnlockEnsure UnlockEnsurePorts| Settings
  App -->|BindUnlockEnsure UnlockEnsurePorts| Contacts
  App -->|BindUnlockEnsure UnlockEnsurePorts| PeoplePicker
  App -->|deferred startup| ClientCompat
  App -->|deferred startup| UnlockGate
  UnlockGate -.->|unlock gate| Settings
  Contacts -->|MessagingContactsPorts| Hub
  PeoplePicker -->|MessagingContactsPorts / MessagingPeoplePickerPorts| Hub
  Shell -->|MessagingShellPorts mesh reads via MeshHost| Hub
  Call -->|CallFunctionalPorts / CallUiBackend → CallStack| Hub
  App -->|owns CallUiBackend bound to Hub::CallStackRef| Call
  Settings -->|ShellNavigationPorts / ShellFeedbackPorts| Shell
  Chat -->|ShellNavigationPorts / ShellFeedbackPorts| Shell
  Contacts -->|ShellNavigationPorts / ShellFeedbackPorts| Shell
  PeoplePicker -->|ShellNavigationPorts / ShellFeedbackPorts| Shell
```

## Settings / prefs hot-reload

Disk DTOs stay in `SessionStore`. Services expose **nested** slice types (`MessagingHub::NetworkConfig`, `ShellHost::ChromePrefs`, …). [`ConfigApplyBridge`](../../src/app/ConfigApplyBridge.h) projects and applies only when a slice changes. Field-level howto: [CONFIGURATION.md](../ops/CONFIGURATION.md).

```mermaid
flowchart TB
  UI["SettingsController<br/><small>gui/ — section Flush</small>"]
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
| Application Run loop | `MessagingHub::TickMesh` | **Removed** — hub policy on coordinator timer (t4) |
| UI presenter | Another controller `::Instance()` | **No** — coordinator or ports ([UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md)) |
| Functional system | `ShellHost::State()` mutation | **No** — UI ports / events |

## Threading

**Canonical doc:** [THREADING.md](THREADING.md) — coordinator + worker pool model, `AppRuntime` API.

UI on main thread; blocking work on **worker pool** via `AppRuntime::PostWorker`; **coordinator** owns timer-driven policy. libp2p and call media run their own loops.

```mermaid
flowchart TB
  subgraph main["Main / UI thread"]
    SDL["Application::Run<br/><small>SDL event loop</small>"]
    UIQ["AppRuntime UI mailbox<br/><small>SequencedTaskRunner — RunUITasks</small>"]
    ShellTick["ShellHost · ChatController<br/><small>feature/ui/shell · feature/ui/chat</small>"]
    SDL --> UIQ
    SDL --> ShellTick
  end

  subgraph coord["Coordinator thread"]
    Coord["CoordinatorThread<br/><small>mailbox + timer wheel</small>"]
    RelayPoll["BackgroundSyncScheduler<br/><small>relay poll 2s/45s</small>"]
    HubPolicy["MessagingHub policy<br/><small>peer sweep · mDNS · reachability</small>"]
    Coord --> RelayPoll
    Coord --> HubPolicy
  end

  subgraph pool["Worker pool 2–4"]
    Pool["WorkerPool<br/><small>Critical · Normal · Background</small>"]
    Http["HttpClient · AgentSession<br/><small>LLM / tools / libcurl</small>"]
    P2pWork["MeshMessagingService · MessagingHub<br/><small>relay sync / send</small>"]
    Pool --> Http
    Pool --> P2pWork
  end

  subgraph libp2p_stack["mesh host"]
    LpIo["Libp2pHost io_thread_<br/><small>asio::io_context::run</small>"]
    Host["libp2p::Host<br/><small>lib/libp2p — Yamux + Noise</small>"]
    LpIo --> Host
  end

  subgraph media_stack["Call media"]
    Cap["CallMediaEngine capture_thread"]
    Vid["CallMediaEngine video_thread"]
    Ring["CallRingtone thread_"]
  end

  UIQ -->|"PostTask(IO) → pool"| Pool
  Pool -->|"PostTask(UI) replies"| UIQ
  Coord -->|"PostWorker blocking steps"| Pool
  HubPolicy -.->|"async dial / streams"| LpIo
```

### Thread inventory

| Thread / queue | Owner class | Location | Role |
|----------------|-------------|----------|------|
| **Main / UI** | `Application` + `AppRuntime` UI mailbox | `app/` · `foundation/runtime/` | SDL loop, RmlUi, shell/chat; drained by `RunUITasks()` |
| **Coordinator** | `CoordinatorThread` | `foundation/runtime/` | Mailbox + timer wheel; relay poll + hub policy |
| **Worker pool** | `WorkerPool` via `AppRuntime` | `common/` · `foundation/runtime/` | HTTP, LLM/tools, relay sync/send |
| **libp2p IO** | `Libp2pHost` | `domain/mesh/` | `asio::io_context` run loop |
| **Media capture / video** | `CallMediaEngine` | `domain/media/` | Dedicated capture + video encode loops |
| **Ringtone** | `CallRingtone` | `domain/media/` | Playback loop thread |
| **Notification watch** | `ILocalNotifier` (Linux) | `foundation/platform/desktop/` | D-Bus watcher; joined in `Shutdown` |

### Cross-thread rules of thumb

- **UI** owns RmlUi and controller mutations. `AppRuntime::PostUI` from pool/coordinator.
- **Worker pool** runs blocking HTTP, LLM, relay orchestration. `PostTaskAndReply` is pool → UI.
- **Coordinator** runs fast policy only; posts blocking steps to pool.
- **libp2p IO** stays non-blocking; integration hops to pool via `PostLibp2pWorker`.
- **Pause/resume:** `AppRuntime::PauseBackgroundWork` / `ResumeBackgroundWork` pauses coordinator + pool.

Full model: [THREADING.md](THREADING.md).

## Notable modules

| Class | Location | Role |
|-------|----------|------|
| **Application** | `app/` | Owns hub, `ProfileSecretsService`, shell, all presenters (`SettingsController`, `ContactsController`, `PeoplePickerController`, `ChatController`, `ShellHost`), `AgentSession`, ActionRouter / ClientCompat / BadgeAggregator / InputCoordinator / FlowCoordinator / CallController / ProfileUnlockGate / PinGate UI; binds ports; installs `ConfigApplyBridge` |
| **SessionStore** | `foundation/data/` | Live disk DTOs; notifies on save/reload |
| **ConfigApplyBridge** | `app/` | Projects nested service slices; fans out `Apply` |
| **MessagingHub** (`MessagingCore`) | `feature/messaging/` | App-only messaging assembler: stores, HTTP Brief clients, inbox/P2P/groups/router, LAN mDNS, policy timers; owns `MeshHost` + `CallStack`; nested network/policy slices |
| **MeshHost** | `domain/mesh/` | Shared mesh composition root (`NodeRuntime` + dial-back + circuit/media relay + reachability). App Hub and headless `pp-node` (`NodeBootstrap`) both own one — not a second libp2p stack |
| **CallStack** | `feature/messaging/` | App-only call plane: media engine, CSM, lifecycle, mesh media bridge, CallMediaDirect, dial/hop helpers; Hub forwards `Calls()` / `Lifecycle()` |
| **MessagingFacade** | `feature/messaging/` | Non-owning wrapper over `MessagingHub&`; app-owned; chat / chat sub-presenters / messaging tools / settings+badge wiring call its methods (no direct hub peeks) |
| **ActionRouter** | `feature/ai/bindings/` | Rml action → tool routing; app-owned |
| **ClientCompatController** | `gui/` | Relay client-compat check; app-owned; deferred startup |
| **BadgeAggregator** | `gui/` | Nav unread badges; app-owned; `BindSource` via `MessagingFacade`; chat via `BadgeNotifyPorts` |
| **BadgeNotifyPorts** | `gui/` | Badge refresh / sessions unread for chat; app-filled from `BadgeAggregator` |
| **InputCoordinator** | `domain/ui/` | Key bindings; app-owned; chat registers Enter-to-send |
| **FlowCoordinator** | `gui/` | Modal overlay dismiss/step-back; app-owned; Shell + PeoplePicker via `FlowCoordinatorPorts` |
| **FlowCoordinatorPorts** | `gui/` | Modal begin/end/dismiss; app-filled from `FlowCoordinator` |
| **CallController** | `gui/` | Call ring / in-call chrome; app-owned; Shell binds for Rml chrome; chat starts/wakes |
| **PinGateController** | `gui/` | PIN overlay presentation; UI ports for ProfileUnlockGate; shell via `PinGateActionPorts` |
| **PinGateActionPorts** | `gui/` | PIN overlay submit/cancel/chooser; app-filled from `PinGateController` |
| **ProfileSecretsService** | `foundation/crypto/` | Profile PIN vault + DEK fan-out; **app-owned** (`unique_ptr` on `Application`; node owns its own in `NodeBootstrap`) — not a singleton; injected into `MessagingHub::BindSecrets`, `ProfileUnlockGate::BindSecrets`, `Bootstrap::Run` |
| **ProfileUnlockGate** | `foundation/crypto/` | Vault unlock policy + caller queue; messaging/UI via ports; presenters via `UnlockEnsurePorts`; secrets via `BindSecrets` |
| **UnlockEnsurePorts** | `gui/` | Ensure unlocked / unlock-in-progress; app-filled from `ProfileUnlockGate` |
| **ShellHost** | `gui/` | Window shell panes/nav; nested `ChromePrefs` |
| **LocalizationService** | `foundation/i18n/` | Locale catalogs; nested `Prefs` |
| **SettingsController** | `gui/` | Me-tab UI + flush via `session_store` port; holds injected `SettingsCommands` only (no messaging bind) |
| **SettingsCommands** | `feature/settings/` | Ports for session, identity, locale, appearance, reachability, PIN status, **Change PIN** (`change_pin` → app-owned vault), imperative ops; app binds implementations |
| **ChatSessionPorts** | `gui/` | Chat nav ports for contacts/people-picker; app-filled from `ChatController` |
| **ContactsNotifyPorts** | `gui/` | Contacts refresh/select for chat; app-filled from `ContactsController` |
| **PeoplePickerNotifyPorts** | `gui/` | Open-picker hooks for chat/call; app-filled from `PeoplePickerController` |
| **CallActionsPorts** | `gui/` | Call chrome/actions for chat, shell, people-picker; app-filled from `CallController` |
| **ProfileIdentityView** | `domain/people/` | Presentation projection of local identity |
| **ChatController** | `gui/chat/` | Chat UI + agent; nested `AgentConfig` |
| **AgentSession** | `feature/ai/` | Turn plan/execute; bound from hub/chat |
| **AppRuntime** | `foundation/runtime/` | UI mailbox + worker pool + coordinator |
| **Libp2pHost** | `domain/mesh/` | Vendored host + asio IO thread |
| **CallMediaEngine** | `domain/media/` | A/V capture threads; encode/decode → libp2p direct or SFU send fn |