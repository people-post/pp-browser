# Feature → domain / split candidates

Confidence tags per [F002](DECISIONS.md#f002--confidence-tags-for-moves). Update when a peel lands or a candidate is demoted.

## sure — peel to domain (f1–f3)

| Files (today) | Target | Notes |
|---------------|--------|-------|
| `feature/messaging/SqlitePskSessionStore.*` | `domain/messaging` | Mirrors other SQLite stores; DEK consumer |
| `feature/messaging/CallMediaKeyStore.*` | `domain/messaging` or `domain/media` | Vault-backed epoch keys |
| `feature/messaging/PskSessionCoordinator.*` | `domain/messaging` | Store ops via contracts |
| `feature/messaging/PublicPskLockCoordinator.*` | `domain/messaging` | Rotate plan/commit |
| `feature/messaging/EpochBumpCoordinator.*` | `domain/messaging` | Thin thread-store policy |
| `feature/messaging/ContactReachability.*` | `domain/people` | Pure eligibility; fixes ui include smell |
| `feature/messaging/PeerBriefRoute.*` | `domain/people` | Route resolution helpers |
| `feature/messaging/ProfileIconFetchUtil.*` | `domain/people` | Cache fetch policy |
| `feature/messaging/MobileEphemeralListenGate.*` | `domain/mesh` | Pure predicate |
| `feature/ui/PeoplePickerLogic.h` | `domain/ui` | Pure logic |
| `feature/ui/CallConflictCopy.*` | `domain/ui` | Copy/CTA policy |

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
