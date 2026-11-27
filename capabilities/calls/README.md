# calls

1:1 and group audio / video calls: the call vocabulary and its store ports (`lib/calls`), the call's sounds
(`lib/call_audio`), the media legs on the mesh (`domain/call_media`) and the session orchestration and composition
root (`feature/calls`: `CallStack`, `CallUiBackend`), on media's engine and devices. Delivery carries its signaling;
it never uses messaging. The first capability made repo-ready ([capability-repos CR023](../../projects/capability-repos/DECISIONS.md#cr023--calls-becomes-repo-ready-first)):
it builds and tests with only what is below it, and reaches its host only through the ports below.

Docs: [docs/README.md](docs/README.md) (architecture, contracts, projects, decisions).

## What it builds on

The capability order ([DESIGN § Target](../../projects/capability-repos/DESIGN.md#target)): **base, people, net,
delivery, pricing, media**, and the pinned libraries (pp-cpp-common, -crypto, -amp, -mesh; SDL3 from pp-cpp-ui for
media's devices; [`cmake/PpPins.cmake`](../../cmake/PpPins.cmake)) and `third_party/` (Opus, curl, zlib, googletest).
Not messaging, social, broadcast, node, nor anything of the app's (`src/`): no include root, no link. The gate
(`scripts/check/check_capability_edges.py`) checks the includes and links, the compiler the include roots (CR022), and
the standalone build the link closure.

| Target | What | Links (besides the base) |
|--------|------|--------------------------|
| `pp_calls_common` | the media leg's health and wire names, shared with broadcast and the app | `pp_media_common` |
| `pp_lib_calls` | the call vocabulary, control codec, store ports, pure path / hop / migration logic | delivery's records, pricing, sodium |
| `pp_lib_calls_testing` | calls' stores in memory (tests only) | `pp_lib_calls` |
| `pp_lib_call_audio` | the ringtone and ringback | media (`pp_lib_media`) |
| `pp_domain_call_media` | the media legs on the mesh's links (bundles, failover, frame crypto) | `pp_lib_calls`, `pp_mesh_shared`, `pp_amp_link` |
| `pp_feature_calls` | sessions, the device, the call stack, the UI backend | media, delivery, pricing, people, `pp_mesh`, `pp_domain_call_media` |

## Build and test it on its own

[`capabilities/CMakeLists.txt`](../CMakeLists.txt) configures chosen capability folders and everything below them,
without the app — what a capability repo's root will be (CR027):

```bash
cmake -S capabilities -B build-calls -DCMAKE_BUILD_TYPE=Debug -DPP_CAPABILITIES=calls
cmake --build build-calls --target pp_capability_calls_tests -j   # calls' tests and what they link
ctest --test-dir build-calls -L '^capability:calls$' -j
```

`cmake --build build-calls` builds the folders below too, with their tests (`ctest --test-dir build-calls` runs them
all). A target that links a `pp_*` name this configure does not define — something above calls — fails configure. The
pins, `-DPP_SIBLINGS_DIR=..` and the other build options work as in the app ([BUILD § Pins and sibling builds](../../docs/ops/BUILD.md#pins-and-sibling-builds)).
Linux and macOS desktop; Windows and the phones build calls through the app. CI's **capabilities** job does this on
every PR that changes calls or anything below it (`build.yml`).

## The host contract

What an app gives calls, all as values and ports; calls asks no platform and no app code of its own. pp-browser's
composition is `feature/product/CallsParticipant` (with `ProductMesh`, `ProductStack`); the probes and
`feature/calls/tests` fill the same ports with fakes.

**Storage and identity**
- `CallStack::InitializeStores(CallStores)`: the call session and media-key stores (`ICallSessionStore`,
  `ICallMediaKeyStore`). The app's are SQLite (`src/lib/storage`, `OpenSqliteCallStores`); in memory:
  `lib/calls/testing`.
- `CallStackDeps`: the chats a call starts from (`IThreadCatalog`), where a summary lands (`IThreadTranscript`), contacts
  (`IContactBook`), this account (`ILocalAccount`), initiation pricing (`IInitiationBillingBook`), the system-control
  registry call control binds into.

**Delivery and the mesh** (`CallStackDeps`)
- `CallDeliveryPorts`: send call control (one peer, many, our own other devices), send a summary into a thread, resolve a
  peer's or a group's thread and its members and standing, grant call peers our relay, sync the inbox on a wake, note a
  missed group call.
- The mesh: the current `MeshHost`, its published `MeshConfig`, the neutral mesh objects (`MeshConnectivity`,
  `MeshMediaRelay`, owned by the host and lent), directory and DHT nodes, node reputation, the seed probe, reach
  prefetch, the relay-chosen hook, the address-disclosure gate.

**Runtime**
- The owner thread calls runs on: `kMediaSessionsOwner` (`common/media/MediaSessionsOwner.h`), which the host registers
  in its `AppRuntimeConfig` (pp-browser: `feature/product/ProductOwners`).
- Text by key through the base's `Localized` port (the host's catalogs answer); call history rows are codes
  (`CallSummaryRow::Outcome`) the host words.

**The UI** (`CallUiBackend`, `CallFunctionalPorts`)
- Intents in (start, accept, decline, mute, video, hold), snapshots and callbacks out (`SetOnRingChanged`,
  `SetOnChromeRefresh`, `SetOnInviteBlocked`); the host draws them. `CallFunctionalPorts`: what the host's call
  controller looks up (threads, contacts, icons, diagnostics on, group invites in a call).
- The ring: `CallRingtone::SetRingSoundPath` (the host resolves its asset), `CallRingtone::OutputPorts` (the phone's
  ringer mode, the vibration).

**The platform — through media** ([`lib/media/AudioSession.h`](../media/src/lib/media/AudioSession.h),
[`lib/media/MediaHost.h`](../media/src/lib/media/MediaHost.h))
- **Audio route:** the host's speaker button drives media's session (`SupportsSpeakerToggle`, `IsSpeakerphoneOn`,
  `SetSpeakerphoneOn`, `ForgetSpeakerChoice` when a call ends); calls' ringback holds the session through it.
- **The call system UI** (CallKit on iOS, the call notification / ConnectionService on Android) is the host's
  (pp-browser: `foundation/platform/CallSystemUi`, the presenters' `CallSystemUiBridge`): it reports a call there and
  answers from there into `CallUiBackend`, and tells media's session who owns the audio session (`SetSystemManaged`,
  `NotifySystemActivation`). Calls never names it.
- **The Android hooks:** media asks the host through `MediaHost` (the call's audio mode, the speakerphone route, whether
  there is an earpiece, retaking the call's audio focus, the device's power and heat), and the host reports into
  `AudioSession::NotifyHostCallAudioFocus` / `NotifyHostResumed`. pp-browser fills them in `src/app/MediaHost_Android.cpp`
  (its activity's methods and native callbacks), installed from `main` before any call; desktop and iOS ask nothing.
- Diagnostics: `AudioSession::SetDiagnosticsEnabled` (the host's choice, at startup).

## Tests

784 tests, label `capability:calls`: units in each folder's `tests/`, and the call stack composed over loopback Amp
stacks with delivery's and calls' in-memory stores (`feature/calls/tests`). Tests that check the app's wording of call
lines stay with the app.
