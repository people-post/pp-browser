# Feature → domain / split candidates

Confidence tags per [F002](DECISIONS.md#f002--confidence-tags-for-moves). Update when a peel lands or a candidate is demoted.

## sure — peel to domain (f1–f3)

Per [F006](DECISIONS.md#f006--sure-peels-use-existing-domain-peers-no-new-peers): **no new top-level peers**; flat drop unless the peer already nests.

| Files (today) | Target path | Notes |
|---------------|-------------|-------|
| `feature/messaging/SqlitePskSessionStore.*` | `domain/messaging/` | Next to `SqliteThreadStore` / `PeerKemKeyStore` |
| `feature/messaging/CallMediaKeyStore.*` | `domain/messaging/` | Next to `CallSessionStore` — **not** `domain/media` |
| `feature/messaging/PskSessionCoordinator.*` | `domain/messaging/` | Next to `PskRotateCodec` |
| `feature/messaging/PublicPskLockCoordinator.*` | `domain/messaging/` | Rotate plan/commit |
| `feature/messaging/EpochBumpCoordinator.*` | `domain/messaging/` | Thin thread-store policy |
| `feature/messaging/ContactReachability.*` | `domain/people/` | Next to `ContactTypes` / `ContactsStore` |
| `feature/messaging/PeerBriefRoute.*` | `domain/people/` | People-centric route helpers |
| `feature/messaging/ProfileIconFetchUtil.*` | `domain/people/` | Next to `ProfileIconCache` |
| `feature/messaging/MobileEphemeralListenGate.*` | `domain/mesh/reachability/` | Only nested home — mesh already bands |
| `feature/ui/PeoplePickerLogic.h` | `domain/ui/` | Pure logic |
| `feature/ui/CallConflictCopy.*` | `domain/ui/` | Copy/CTA policy |

**Not doing for f1–f3:** `domain/calls`, `domain/psk`, new `pp_domain_*` targets, or a messaging internal subfolder migration.

## likely — after a short peel

| Files | Target | Blocker |
|-------|--------|---------|
| `feature/messaging/ChatHistoryResponder.*` | `domain/messaging` | Drop / inject identity params (mirror `ChatBlobResponder`) |
| `feature/messaging/IDirectMessageClient.h` | `common/` | Iface only; impl stays |
| Amp mesh-only façades (`AmpCircuitHopReach`, `AmpMediaRelayClient`) | `domain/mesh` | Audit: no messaging-store includes |

## blocked — stay feature (or need common first)

| Files | Why |
|-------|-----|
| `DirectoryShadowCache.*` | net client + people DTOs |
| `GroupInviteGate.*` | people + messaging stores |
| `PeerDisplayResolver.*` | people + roster + shadow cache |
| `RegistrationClientUtil.*` | net + people (+ UI labels) |
| `AttachmentClientUtil.*`, `AttachmentFetchUtil.*`, `ChatBlobRequestUtil.*` | multi-peer ladders |
| `AttachmentDownloadService.*` | orchestration queue |
| `ProfileIconClientUtil.*` | blob upload + identity |
| `RelayDirectory*KeyResolver.*` | messaging store + directory |
| `AmpDirectChatService.*`, `AmpChatHistoryService.*`, `AmpChatBlobService.*` | product Amp adapters (unless audit proves mesh-only) |
| Hubs / pipelines / routers / controllers / ports | orchestration or UI seams |

## structural — folder splits (f4–f5)

| Move | Depends on | Notes |
|------|------------|-------|
| Extract `feature/calls` | f1–f2; [F004](DECISIONS.md#f004--reserved-calls-module-home) | CallStack, CSM, Lifecycle, Topology, CallUiBackend, ports |
| Extract `feature/shell` | f2 optional; [F005](DECISIONS.md#f005--reserved-shell--contacts-split) | ShellHost, gestures, feedback, RmlMount, chrome sync |
| Extract `feature/contacts` | f2 (`ContactReachability`) | Contacts + PeoplePicker |
| App named wirers | anytime after f0 | No behavior change |
| ChatController / Inbox presentation split | f6 last | Highest product risk |

## stay feature (orchestration) — do not lower

`MessagingHub`, `MessagingFacade`, `MeshMessagingService`, `RelayReceivePipeline`, `ChatSyncService`, `MessageRouter`, `InboxController`, `GroupMembershipService`, `ContactActionDispatcher`, `PushDeviceCoordinator`, `LinkDeviceCoordinator`, all `*Ports*`, UI controllers, `AgentSession`, settings apply orchestration.
