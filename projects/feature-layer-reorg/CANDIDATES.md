# Feature → domain / split candidates

Confidence tags per [F002](DECISIONS.md#f002--confidence-tags-for-moves). Update when a peel lands or a candidate is demoted.

## sure — peeled to domain (f1–f2 done)

Per [F006](DECISIONS.md#f006--sure-peels-use-existing-domain-peers-no-new-peers): **no new top-level peers**; flat drop unless the peer already nests.

| Files (now) | Home | Notes |
|-------------|------|-------|
| `domain/messaging/SqlitePskSessionStore.*` | `domain/messaging/` | Done |
| `domain/messaging/CallMediaKeyStore.*` | `domain/messaging/` | Done — not `domain/media` |
| `domain/messaging/PskSessionCoordinator.*` | `domain/messaging/` | Done |
| `domain/messaging/PublicPskLockCoordinator.*` | `domain/messaging/` | Done; feature test still covers LinkDevice export case |
| `domain/messaging/EpochBumpCoordinator.*` | `domain/messaging/` | Done |
| `domain/people/ContactReachability.*` | `domain/people/` | Done |
| `domain/people/PeerBriefRoute.*` | `domain/people/` | Done |
| `domain/mesh/reachability/MobileEphemeralListenGate.*` | `domain/mesh/reachability/` | Done |
| `domain/ui/PeoplePickerLogic.h` | `domain/ui/` | Done |
| `domain/ui/CallConflictCopy.*` | `domain/ui/` | Done |

### Demoted during f2

| Files | Why |
|-------|-----|
| `feature/messaging/ProfileIconFetchUtil.*` | Uses `HttpClient` — people→net banned; keep in feature until download is injected via common/net port |

**Not doing for f1–f3:** `domain/calls`, `domain/psk`, new `pp_domain_*` targets, or a messaging internal subfolder migration.

## likely — after a short peel

| Files | Target | Blocker |
|-------|--------|---------|
| Amp mesh-only façades (`AmpCircuitHopReach`, `AmpMediaRelayClient`) | `domain/mesh` | Audit: no messaging-store includes; better with f4v1 calls band |

### Done in f3

| Files | Home | Notes |
|-------|------|-------|
| `common/chat/IDirectMessageClient.h` | `common/chat/` | Dropped unused `domain/net` include |
| `domain/messaging/ChatHistoryResponder.*` | `domain/messaging/` | `IdentityStore` replaced by account id + `SignBytesFn` |

## blocked — stay feature (or need common first)

| Files | Why |
|-------|-----|
| `DirectoryShadowCache.*` | net client + people DTOs |
| `GroupInviteGate.*` | people + messaging stores |
| `PeerDisplayResolver.*` | people + roster + shadow cache |
| `RegistrationClientUtil.*` | net + people (+ UI labels) |
| `ProfileIconFetchUtil.*` | people cache + `HttpClient` (people→net) |
| `AttachmentClientUtil.*`, `AttachmentFetchUtil.*`, `ChatBlobRequestUtil.*` | multi-peer ladders |
| `AttachmentDownloadService.*` | orchestration queue |
| `ProfileIconClientUtil.*` | blob upload + identity |
| `RelayDirectory*KeyResolver.*` | messaging store + directory |
| `AmpDirectChatService.*`, `AmpChatHistoryService.*`, `AmpChatBlobService.*` | product Amp adapters (unless audit proves mesh-only) |
| Hubs / pipelines / routers / controllers / ports | orchestration or UI seams |

## structural — folder splits (f4–f6)

| Move | Depends on | Notes |
|------|------------|-------|
| Nest `feature/messaging/calls/` | f1–f3; [F004](DECISIONS.md#f004--calls-home-nested-band-first-then-top-level) | Same CMake target |
| Top-level `feature/calls` | After delivery ports break Hub↔CSM cycle | End-state name per [F007](DECISIONS.md#f007--vocabulary--end-state-feature-names) |
| Rename `messaging` → `conversations` | After hub ownership clean | [F007](DECISIONS.md#f007--vocabulary--end-state-feature-names) |
| Extract `feature/shell` / `contacts` | f5 | From ui grab-bag |
| Absorb `feature/chat` → `feature/ui` | f5 | **No** top-level chat in end state ([F007](DECISIONS.md#f007--vocabulary--end-state-feature-names)) |
| App named wirers | anytime | `WireConversations` / `WireCalls` |
| Inbox presentation split | f6 last | Highest product risk |

## stay feature (orchestration) — do not lower

`MessagingHub`, `MessagingFacade`, `MeshMessagingService`, `RelayReceivePipeline`, `ChatSyncService`, `MessageRouter`, `InboxController`, `GroupMembershipService`, `ContactActionDispatcher`, `PushDeviceCoordinator`, `LinkDeviceCoordinator`, all `*Ports*`, UI controllers, `AgentSession`, settings apply orchestration.
