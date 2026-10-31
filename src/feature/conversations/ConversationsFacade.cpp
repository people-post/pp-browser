#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/GroupMentions.h"

#include <algorithm>

#include "foundation/data/PricingTypes.h"
#include "foundation/runtime/AppRuntime.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/pricing/InitiationPricing.h"
#include "domain/registration/RegistrationClient.h"
#include "feature/conversations/PeopleSearch.h"
#include "lib/people/LinkDeviceCoordinator.h"
#include "foundation/crypto/RecoveryPhrase.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "feature/conversations/ConversationsHub.h"
#include "feature/conversations/ContactCardExchange.h"
#include "lib/messaging/ContactLocator.h"
#include "feature/conversations/PushDeviceCoordinator.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"


namespace pbr {

ConversationsFacade::ConversationsFacade(ConversationsHub& hub) : hub_(hub) {}

// --- Core / lifecycle -------------------------------------------------------

MessagingView ConversationsFacade::Snapshot() { return ProjectMessagingView(hub_); }

bool ConversationsFacade::HasRouter() { return hub_.HasRouter(); }

bool ConversationsFacade::IsInitialized() { return hub_.IsInitialized(); }

bool ConversationsFacade::IsMessagingReady() { return hub_.IsMessagingReady(); }

bool ConversationsFacade::IsMeshReady() { return hub_.IsMeshReady(); }

IThreadStore* ConversationsFacade::ThreadStore() { return &hub_.Store(); }

void ConversationsFacade::BindAgentInbound(AgentInboundPorts ports) {
  hub_.BindAgentInbound(std::move(ports));
}

Roe<bool> ConversationsFacade::MaybeAutoRenewRegistration(const bool auto_renew_registration) {
  return pbr::MaybeAutoRenewRegistration(hub_.Registration(), hub_.Identity(), auto_renew_registration);
}

Roe<void> ConversationsFacade::SyncPushDevices() {
  return PushDeviceCoordinator::Register(hub_);
}

Roe<std::optional<std::string>> ConversationsFacade::RecoveryPhrase(const std::string& pin) {
  if (!hub_.IsInitialized() || hub_.Secrets() == nullptr || hub_.Secrets()->Vault() == nullptr) {
    return Error("Messaging is not ready");
  }
  // Holding the unlocked app is not enough: the phrase is the whole account. The PIN must open the vault again.
  DataKeyVault& vault = *hub_.Secrets()->Vault();
  auto vault_bytes = vault.ReadVaultFile();
  if (!vault_bytes) {
    return vault_bytes.error();
  }
  if (auto opened = DataKeyVault::UnwrapDekFromVaultBytes(*vault_bytes, vault.ProfileId(), pin); !opened) {
    return opened.error();
  }
  auto seed = hub_.Identity().RecoverySeed();
  if (!seed) {
    return seed.error();
  }
  if (!*seed) {
    return std::optional<std::string>{};
  }
  auto phrase = EncodeRecoveryPhrase(**seed);
  if (!phrase) {
    return phrase.error();
  }
  return std::optional<std::string>(std::move(*phrase));
}

bool ConversationsFacade::HasRecoveryPhrase() {
  if (!hub_.IsInitialized() || hub_.Secrets() == nullptr || !hub_.Secrets()->IsUnlocked()) {
    return false;
  }
  auto seed = hub_.Identity().RecoverySeed();
  return seed && seed->has_value();
}

Roe<std::string> ConversationsFacade::ExportLinkDevice() {
  if (!hub_.IsInitialized() || hub_.Secrets() == nullptr || hub_.Secrets()->Vault() == nullptr) {
    return Error("Messaging is not ready");
  }
  // M021 d4: the master's transfer code is its consent — the new device appends itself to the master's list.
  auto devices = hub_.OwnDevices();
  if (!devices) {
    return devices.error();
  }
  if (devices->signed_out) {
    return Error(kSignedOutMessage);
  }
  if (!devices->devices.empty() && devices->devices.front().peer_id != devices->self_peer_id) {
    return Error("Only your main device can add a device");
  }
  std::optional<std::string> card = hub_.OwnAccountCardJson();
  if (!card) {
    return Error("This device has no account card yet");
  }
  const int64_t now = util::NowUnixMs();
  auto bundle = LinkDeviceCoordinator::ExportJson(hub_.Identity(), *hub_.Secrets()->Vault(), std::move(*card), now);
  if (bundle) {
    hub_.ExpectDeviceJoin(now + kLinkDeviceDefaultTtlMs);
  }
  return bundle;
}

Roe<void> ConversationsFacade::ImportLinkDevice(const std::string& bundle_json, const std::string& pin) {
  if (!hub_.IsInitialized() || hub_.Secrets() == nullptr || hub_.Secrets()->Vault() == nullptr) {
    return Error("Messaging is not ready");
  }
  auto imported = LinkDeviceCoordinator::Import(hub_.Identity(), *hub_.Secrets()->Vault(), hub_.Secrets(),
                                                bundle_json, pin, util::NowUnixMs());
  if (!imported) {
    return imported.error();
  }
  if (imported->account_card_json) {
    // The account's device list as the linking device knows it; a failure (logged) leaves this device to
    // start a list of its own (its siblings learn it from its card later), not a failed link.
    (void)hub_.LearnLinkedAccountCard(*imported->account_card_json);
  }
  return {};
}

Roe<void> ConversationsFacade::RestoreFromRecoveryPhrase(const std::string& phrase, const std::string& pin,
                                                         const std::vector<std::string>& saved_copies) {
  return hub_.RestoreFromRecoveryPhrase(phrase, pin, saved_copies);
}

Roe<OwnDeviceList> ConversationsFacade::OwnDevices() { return hub_.OwnDevices(); }

Roe<void> ConversationsFacade::RemoveOwnDevice(const std::string& peer_id) { return hub_.RemoveOwnDevice(peer_id); }

Roe<void> ConversationsFacade::MakeMainDevice(const std::string& peer_id) { return hub_.MakeMainDevice(peer_id); }

Roe<std::string> ConversationsFacade::ExportAccountCopy() {
  AccountLockerWorkflow* locker = hub_.AccountLocker();
  if (!hub_.IsMessagingReady() || !locker) {
    return Error("Messaging is not ready");
  }
  return locker->ExportFileJson();
}

void ConversationsFacade::SuspendMeshColdPeers() { hub_.SuspendMeshColdPeers(); }

// --- Inbox ------------------------------------------------------------------

const std::string& ConversationsFacade::ActiveThreadId() { return hub_.Inbox().ActiveThreadId(); }

Roe<Thread> ConversationsFacade::GetActiveThread() { return hub_.Inbox().GetActiveThread(); }

void ConversationsFacade::IncrementUnread(const std::string& thread_id) {
  hub_.Inbox().IncrementUnread(thread_id, 1, /*even_if_active=*/true); // the caller checked it is not on screen
}

Roe<std::vector<Thread>> ConversationsFacade::ListThreads() { return hub_.Inbox().ListThreads(); }

bool ConversationsFacade::ThreadHasActiveCall(const std::string& thread_id) {
  if (thread_id.empty() || !active_call_query_) {
    return false;
  }
  auto thread = GetThread(thread_id);
  if (!thread || !*thread) {
    return false;
  }
  return active_call_query_(**thread);
}

void ConversationsFacade::SetActiveCallQuery(std::function<bool(const Thread& thread)> query) {
  active_call_query_ = std::move(query);
}

Roe<Thread> ConversationsFacade::OpenThread(const std::string& thread_id) { return hub_.Inbox().OpenThread(thread_id); }

Roe<void> ConversationsFacade::CloseThread(const std::string& thread_id) {
  auto closed = hub_.Inbox().CloseThread(thread_id);
  if (closed) {
    (void)hub_.ThreadPrefs().Delete(thread_id); // a deleted thread keeps no pin / mute / archive row
  }
  return closed;
}

void ConversationsFacade::ClearActiveThread() { hub_.Inbox().ClearActiveThread(); }

Roe<Thread> ConversationsFacade::CreateNewAiThread() { return hub_.Inbox().CreateNewAiThread(); }

Roe<void> ConversationsFacade::UpdatePreview(const std::string& thread_id, const std::string& preview) {
  return hub_.Inbox().UpdatePreview(thread_id, preview);
}

Roe<void> ConversationsFacade::DeleteMessageForMe(const std::string& thread_id, const std::string& message_id) {
  hub_.MeshMessaging().CancelPendingSend(message_id);
  return hub_.Inbox().DeleteMessageForMe(thread_id, message_id);
}

Roe<std::optional<ThreadMessage>> ConversationsFacade::GetMessage(const std::string& thread_id,
                                                                  const std::string& message_id) {
  return hub_.Store().GetMessage(thread_id, message_id);
}

Roe<void> ConversationsFacade::DeleteMessageForEveryone(const std::string& thread_id, const std::string& message_id) {
  return hub_.DeleteMessageForEveryone(thread_id, message_id);
}

Roe<void> ConversationsFacade::ClearThreadHistory(const std::string& thread_id, const bool forget_memory) {
  return hub_.Inbox().ClearThreadHistory(thread_id, forget_memory);
}

Roe<void> ConversationsFacade::ForgetThreadMemory(const std::string& thread_id) {
  return hub_.Inbox().ForgetThreadMemory(thread_id);
}

Roe<void> ConversationsFacade::SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title) {
  return hub_.Inbox().SetThreadLocalTitle(thread_id, local_title);
}

Roe<void> ConversationsFacade::NameAiThreadFromFirstMessage(const std::string& thread_id, const std::string& title) {
  return hub_.Inbox().NameAiThreadFromFirstMessage(thread_id, title);
}

Roe<Thread> ConversationsFacade::FindOrCreateDirectThread(const std::string& contact_id, const ThreadChannel channel) {
  return hub_.Inbox().FindOrCreateDirectThread(contact_id, channel);
}

Roe<std::vector<ThreadLocalPrefs>> ConversationsFacade::ListThreadLocalPrefs() { return hub_.ThreadPrefs().List(); }

Roe<ThreadLocalPrefs> ConversationsFacade::GetThreadLocalPrefs(const std::string& thread_id) {
  return hub_.ThreadPrefs().Get(thread_id);
}

Roe<void> ConversationsFacade::SetThreadLocalPrefs(const ThreadLocalPrefs& prefs) {
  return hub_.ThreadPrefs().Set(prefs);
}

void ConversationsFacade::MarkThreadRead(const std::string& thread_id) { hub_.Inbox().MarkThreadRead(thread_id); }

void ConversationsFacade::NotifyThreadChanged() { hub_.Inbox().NotifyThreadChanged(); }

void ConversationsFacade::SetOnThreadChanged(std::function<void()> callback) {
  hub_.Inbox().SetOnThreadChanged(std::move(callback));
}

PeerDisplayLabel ConversationsFacade::ResolveThreadLabel(const Thread& thread) {
  return hub_.Inbox().ResolveThreadLabel(thread);
}

std::vector<TranscriptRow> ConversationsFacade::BuildTranscript(const std::string& thread_id,
                                                            const std::optional<int64_t> oldest_inclusive,
                                                            const std::optional<int64_t> newest_inclusive) {
  return hub_.Inbox().BuildTranscript(thread_id, oldest_inclusive, newest_inclusive);
}

TranscriptFacts ConversationsFacade::TranscriptFactsFor() {
  TranscriptFacts facts = hub_.Inbox().Facts();
  facts.member_name = [this](const std::string& account_id) { return MemberPublicNameOf(account_id); };
  if (auto identity = GetIdentity()) {
    facts.local_account = identity->account_id;
  }
  return facts;
}

std::string ConversationsFacade::MemberPublicNameOf(const std::string& account_id) {
  std::optional<Contact> contact;
  if (auto found = FindContactByIdentity(account_id, mesh::ContactIdKind::Account); found && *found) {
    contact = **found;
  }
  return MemberPublicName(account_id, KeptCardNickname(account_id), contact);
}

const std::string& ConversationsFacade::ProfileDataDir() const {
  return hub_.ProfileDataDir();
}

bool ConversationsFacade::HasLocalMessagesBefore(const std::string& thread_id, const int64_t before_display_order) {
  return hub_.Inbox().HasLocalMessagesBefore(thread_id, before_display_order);
}

int ConversationsFacade::SumUnread() { return hub_.Inbox().SumUnread(); }

// --- Store ------------------------------------------------------------------

Roe<std::optional<Thread>> ConversationsFacade::GetThread(const std::string& thread_id) {
  return hub_.Store().GetThread(thread_id);
}

Roe<std::vector<ThreadMessage>> ConversationsFacade::GetMessagesPage(const std::string& thread_id,
                                                                const std::optional<int64_t> before,
                                                                const int limit) {
  return hub_.Store().GetMessagesPage(thread_id, before, limit);
}

Roe<ThreadMessage> ConversationsFacade::AppendMessage(const ThreadMessage& message) {
  return hub_.Store().AppendMessage(message);
}

Roe<bool> ConversationsFacade::UpdateMessage(const ThreadMessage& message) {
  return hub_.Store().UpdateMessage(message);
}

Roe<uint32_t> ConversationsFacade::GetChatTargetSessionEpoch(const std::string& thread_id) {
  return hub_.Store().GetChatTargetSessionEpoch(thread_id);
}

Roe<PeerSyncState> ConversationsFacade::GetPeerSyncState(const std::string& thread_id, const uint32_t epoch) {
  return hub_.Store().GetPeerSyncState(thread_id, epoch);
}

// --- P2P --------------------------------------------------------------------

void ConversationsFacade::MaybeTailSync(const std::string& thread_id) { hub_.MeshMessaging().MaybeTailSync(thread_id); }

Roe<ThreadMessage> ConversationsFacade::SendUserMessage(const std::string& thread_id, const std::string& text,
                                                    SendRelayOptions opts) {
  return hub_.MeshMessaging().SendUserMessage(thread_id, text, opts);
}

Roe<ThreadMessage> ConversationsFacade::SendGroupMessage(const std::string& thread_id, const std::string& text,
                                                         SendRelayOptions opts) {
  return hub_.MeshMessaging().SendGroupMessage(thread_id, text, opts);
}

Roe<ThreadMessage> ConversationsFacade::SendGroupText(const std::string& thread_id, const std::string& text,
                                                      std::vector<TextSpan> spans) {
  if (auto checked = text_spans::CheckStructure(spans); !checked) {
    return checked.error();
  }
  spans = text_spans::KeepValidRanges(text, std::move(spans));
  SendRelayOptions opts;
  if (!spans.empty()) {
    opts.payload_json = text_spans::ToPayloadJson(text, spans);
  }
  return hub_.MeshMessaging().SendGroupMessage(thread_id, text, opts);
}

std::set<std::string> ConversationsFacade::ThreadsWithMentions() {
  ThreadMentionsStore* mentions = hub_.ThreadMentions();
  if (!mentions) {
    return {};
  }
  auto threads = mentions->Threads();
  return threads ? std::move(*threads) : std::set<std::string>{};
}

std::optional<std::string> ConversationsFacade::OldestMention(const std::string& thread_id) {
  ThreadMentionsStore* mentions = hub_.ThreadMentions();
  if (!mentions) {
    return std::nullopt;
  }
  auto oldest = mentions->Oldest(thread_id);
  return oldest ? *oldest : std::nullopt;
}

Roe<ThreadMessage> ConversationsFacade::ShareContact(const std::string& thread_id, const std::string& contact_id) {
  return hub_.ShareContact(thread_id, contact_id);
}

Roe<std::string> ConversationsFacade::OwnContactLocator() {
  return hub_.OwnContactLocator();
}

void ConversationsFacade::AddContactFromLocator(const std::string& text,
                                                std::function<void(Roe<ContactCardExchange::LocatorAdded>)> done) {
  if (!hub_.IsInitialized()) {
    done(Error("Messaging not ready"));
    return;
  }
  auto locator = FindContactLocator(text);
  if (!locator) {
    done(locator.error());
    return;
  }
  hub_.ContactCards().AddFromLocator(*locator, std::move(done));
}

Roe<Contact> ConversationsFacade::AddContactFromCard(const std::string& thread_id, const std::string& message_id) {
  if (!hub_.IsInitialized()) {
    return Error("Messaging not ready");
  }
  auto message = GetMessage(thread_id, message_id);
  if (!message || !*message || (*message)->content_type != ChatContentType::ContactCard) {
    return Error("Not a contact card");
  }
  auto fields = ChatPayloadCodec::DecodeContactCardJson((*message)->payload_json);
  if (!fields) {
    return fields.error();
  }
  return hub_.ContactCards().Add(*fields, util::NowUnixMs());
}

Roe<ThreadMessage> ConversationsFacade::SendReaction(const std::string& thread_id, const std::string& target_message_id,
                                                 const std::string& emoji) {
  return hub_.MeshMessaging().SendReaction(thread_id, target_message_id, emoji);
}

Roe<ThreadMessage> ConversationsFacade::ClearReaction(const std::string& thread_id, const std::string& target_message_id,
                                                  const std::string& emoji) {
  return hub_.MeshMessaging().ClearReaction(thread_id, target_message_id, emoji);
}

void ConversationsFacade::SetOnMessagesChanged(std::function<void()> callback) {
  hub_.MeshMessaging().SetOnMessagesChanged(std::move(callback));
}

void ConversationsFacade::SetOnDeliveryNotice(std::function<void(const DeliveryNotice&)> callback) {
  hub_.MeshMessaging().SetOnDeliveryNotice(std::move(callback));
}

void ConversationsFacade::SetOnBackgroundUnread(std::function<void(std::string, std::string, std::string, bool)> callback) {
  hub_.MeshMessaging().SetOnBackgroundUnread(std::move(callback));
}

void ConversationsFacade::SyncInboxFromWake(const bool force) { hub_.MeshMessaging().SyncInboxFromWake(force); }

void ConversationsFacade::TailSyncActiveE2eThread() { hub_.MeshMessaging().TailSyncActiveE2eThread(); }

void ConversationsFacade::WarmPeerForThread(const std::string& thread_id) { hub_.MeshMessaging().WarmPeerForThread(thread_id); }

ThreadPeerLinkView ConversationsFacade::GetThreadPeerLink(const std::string& thread_id) {
  return hub_.MeshMessaging().GetThreadPeerLink(thread_id);
}

void ConversationsFacade::RetryPeerDial(const std::string& thread_id) { hub_.MeshMessaging().RetryPeerDial(thread_id); }

void ConversationsFacade::ScrollBackfill(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done) {
  hub_.MeshMessaging().ScrollBackfill(thread_id, std::move(on_done));
}

void ConversationsFacade::SyncWithPeer(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done) {
  hub_.MeshMessaging().SyncWithPeer(thread_id, std::move(on_done));
}

void ConversationsFacade::RetryGapSync(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done) {
  hub_.MeshMessaging().RetryGapSync(thread_id, std::move(on_done));
}

Roe<uint32_t> ConversationsFacade::StartNewSecureChat(const std::string& thread_id) {
  return hub_.MeshMessaging().StartNewSecureChat(thread_id);
}

Roe<void> ConversationsFacade::PauseIntegrityOnly(const std::string& thread_id) {
  return hub_.MeshMessaging().PauseIntegrityOnly(thread_id);
}

Roe<PskExportView> ConversationsFacade::EnsurePskGenerated(const std::string& thread_id) {
  return hub_.MeshMessaging().EnsurePskGenerated(thread_id);
}

Roe<PskExportView> ConversationsFacade::GetPskExportView(const std::string& thread_id) {
  return hub_.MeshMessaging().GetPskExportView(thread_id);
}

Roe<PskSessionStatus> ConversationsFacade::GetPskStatus(const std::string& thread_id) {
  return hub_.MeshMessaging().GetPskStatus(thread_id);
}

Roe<void> ConversationsFacade::ImportPskBundleJson(const std::string& thread_id, const std::string& bundle_json) {
  return hub_.MeshMessaging().ImportPskBundleJson(thread_id, bundle_json);
}

Roe<void> ConversationsFacade::ImportPskRawBase64(const std::string& thread_id, const std::string& raw_b64) {
  return hub_.MeshMessaging().ImportPskRawBase64(thread_id, raw_b64);
}

Roe<void> ConversationsFacade::MarkPskVerified(const std::string& thread_id) {
  return hub_.MeshMessaging().MarkPskVerified(thread_id);
}

Roe<std::string> ConversationsFacade::RotatePskAndExportBundle(const std::string& thread_id) {
  return hub_.MeshMessaging().RotatePskAndExportBundle(thread_id);
}

Roe<void> ConversationsFacade::LockPublicThreadToThisDevice(const std::string& thread_id) {
  return hub_.MeshMessaging().LockPublicThreadToThisDevice(thread_id);
}

Roe<PublicKeyScope> ConversationsFacade::GetPublicKeyScope(const std::string& thread_id) {
  return hub_.MeshMessaging().GetPublicKeyScope(thread_id);
}

Roe<bool> ConversationsFacade::CanLockPublicToThisDevice(const std::string& thread_id) {
  return hub_.MeshMessaging().CanLockPublicToThisDevice(thread_id);
}

void ConversationsFacade::SetSupportAccountId(std::string account_id) {
  hub_.MeshMessaging().SetSupportAccountId(std::move(account_id));
}

void ConversationsFacade::RegisterContactDirectEndpoints(const Contact& contact) {
  hub_.MeshMessaging().RegisterContactDirectEndpoints(contact);
}

void ConversationsFacade::RegisterPeerKeysFromDirectory(const mesh::DirectoryHit& hit) {
  hub_.MeshMessaging().RegisterPeerKeysFromDirectory(hit);
}

Roe<Contact> ConversationsFacade::AddContactFromDirectoryHit(const mesh::DirectoryHit& hit) {
  return hub_.Contacts().AddFromDirectoryHit(hit);
}

Roe<void> ConversationsFacade::SetContactTrust(const std::string& contact_id, const TrustLevel level) {
  auto contact = hub_.Contacts().Get(contact_id);
  if (!contact) {
    return contact.error();
  }
  if (!*contact) {
    return Error("Contact not found");
  }
  Contact updated = **contact;
  updated.local.trust = level;
  SyncContactMirrors(updated);
  if (auto saved = hub_.Contacts().Upsert(updated); !saved) {
    return saved.error();
  }
  return {};
}

Roe<RelayDeleteResult> ConversationsFacade::ClearUndeliveredOlderThan(const int older_than_days) {
  return hub_.MeshMessaging().ClearUndeliveredOlderThan(older_than_days);
}

// --- Groups -----------------------------------------------------------------

Roe<void> ConversationsFacade::DismissLocalGroup(const std::string& group_id) {
  return hub_.Groups().DismissLocalGroup(group_id);
}

Roe<bool> ConversationsFacade::IsLocalOwner(const std::string& group_id) { return hub_.Groups().IsLocalOwner(group_id); }

Roe<std::vector<GroupRosterMember>> ConversationsFacade::ListGroupRoster(const std::string& group_id) {
  return hub_.Groups().ListRoster(group_id);
}

bool ConversationsFacade::IsMemberUnreachable(const std::string& group_id, const std::string& member_identity) {
  return hub_.Groups().IsMemberUnreachable(group_id, member_identity);
}

Roe<void> ConversationsFacade::LeaveGroup(const std::string& group_id) { return hub_.Groups().LeaveGroup(group_id); }

Roe<void> ConversationsFacade::LeaveAsOwner(const std::string& group_id, const std::string& new_owner_identity) {
  return hub_.Groups().LeaveAsOwner(group_id, new_owner_identity);
}

Roe<std::string> ConversationsFacade::OwnerIdentity(const std::string& group_id) {
  return hub_.Groups().OwnerIdentity(group_id);
}

std::string ConversationsFacade::KeptCardNickname(const std::string& account_id) {
  return hub_.KeptCardNickname(account_id);
}

bool ConversationsFacade::IsOwnerUnreachable(const std::string& group_id) {
  return hub_.Groups().IsOwnerUnreachable(group_id);
}

Roe<void> ConversationsFacade::RenameGroupShared(const std::string& group_id, const std::string& title) {
  return hub_.Groups().RenameGroupShared(group_id, title);
}

Roe<void> ConversationsFacade::RemoveMemberByIdentity(const std::string& group_id, const std::string& member_identity) {
  return hub_.Groups().RemoveMemberByIdentity(group_id, member_identity);
}

std::vector<std::string> ConversationsFacade::ListUnreachableMembers(const std::string& group_id) {
  return hub_.Groups().ListUnreachable(group_id);
}

Roe<void> ConversationsFacade::InviteMember(const std::string& group_id, const std::string& contact_id) {
  auto roster = hub_.Groups().ListRoster(group_id);
  if (!roster) {
    return roster.error();
  }
  // A re-invite (already on the roster) takes no new seat.
  std::vector<std::string> contact_keys{contact_id};
  if (auto contact = GetContact(contact_id); contact && *contact) {
    for (const mesh::ContactId& id : (*contact)->ids) {
      contact_keys.push_back(id.value);
    }
  }
  const bool on_roster = std::any_of(roster->begin(), roster->end(), [&](const GroupRosterMember& member) {
    return std::find(contact_keys.begin(), contact_keys.end(), member.contact_id) != contact_keys.end() ||
           std::find(contact_keys.begin(), contact_keys.end(), member.member_identity) != contact_keys.end();
  });
  if (!on_roster) {
    if (auto room = CheckGroupHasRoom(roster->size()); !room) {
      return room.error();
    }
  }
  return hub_.Groups().InviteMember(group_id, contact_id);
}

void ConversationsFacade::InviteMembersAsync(const std::string& group_id, std::vector<std::string> contact_ids,
                                             std::function<void(GroupInviteBatchResult)> done) {
  AppRuntime::PostWorkerAndReplyOnUI<GroupInviteBatchResult>(
      WorkerLane::Normal,
      [this, group_id, contact_ids = std::move(contact_ids)]() {
        return InviteEach(contact_ids, [&](const std::string& contact_id) -> Roe<void> {
          // Locked or quitting while the batch runs: the rest are not sent.
          if (AppRuntime::IsShuttingDown() || !hub_.IsInitialized()) {
            return Error("Messaging not ready");
          }
          return InviteMember(group_id, contact_id);
        });
      },
      [done = std::move(done)](GroupInviteBatchResult result) {
        if (done) {
          done(std::move(result));
        }
      });
}

// --- Identity / contacts / directory ----------------------------------------

std::optional<LocalIdentity> ConversationsFacade::GetIdentity() {
  if (auto identity = hub_.Identity().Get()) {
    return *identity;
  }
  return std::nullopt;
}

Roe<std::optional<Contact>> ConversationsFacade::FindContactByIdentity(const std::string& identity,
                                                                  const mesh::ContactIdKind kind) {
  return hub_.Contacts().FindByIdentity(identity, kind);
}

Roe<std::optional<Contact>> ConversationsFacade::GetContact(const std::string& contact_id) {
  return hub_.Contacts().Get(contact_id);
}

std::optional<mesh::DirectoryHit> ConversationsFacade::GetDirectoryShadow(const std::string& peer_id) {
  return hub_.DirectoryShadows().Get(peer_id);
}

void ConversationsFacade::EnsureDirectoryLookup(const std::string& peer_id) {
  hub_.DirectoryShadows().EnsureLookup(peer_id);
}

// --- Actions ----------------------------------------------------------------

Roe<std::optional<std::string>> ConversationsFacade::DispatchAction(const std::string& payload_json) {
  return hub_.Actions().Dispatch(payload_json);
}

void ConversationsFacade::DispatchActionAsync(const std::string& payload_json,
                                              std::function<void(Roe<std::optional<std::string>>)> done) {
  hub_.Actions().DispatchAsync(payload_json, std::move(done));
}

void ConversationsFacade::SetOnActionMessage(std::function<void(const std::string& message)> callback) {
  hub_.Actions().SetOnActionMessage(std::move(callback));
}

// --- Router -----------------------------------------------------------------

Roe<void> ConversationsFacade::RouteMessage(const std::string& thread_id, const std::string& text,
                                        std::optional<std::string> user_payload) {
  if (!hub_.HasRouter()) {
    return Error("Messaging not ready");
  }
  return hub_.Router().Route(thread_id, text, std::move(user_payload));
}

bool ConversationsFacade::ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                                       const std::optional<std::string>& user_payload) {
  return hub_.HasRouter() && hub_.Router().ExpectsAgentWork(thread_id, text, user_payload);
}

void ConversationsFacade::SetOnLocalAction(
    std::function<void(const std::string&, const std::optional<std::string>&)> callback) {
  hub_.Router().SetOnLocalAction(std::move(callback));
}

void ConversationsFacade::SetSharedAiConfirmCallback(MessageRouter::SharedAiConfirmCallback callback) {
  hub_.Router().SetSharedAiConfirmCallback(std::move(callback));
}

Roe<void> ConversationsFacade::RouteConfirmedSharedAi(const std::string& thread_id, const std::string& text) {
  if (!hub_.HasRouter()) {
    return Error("Router not ready");
  }
  return hub_.Router().RouteConfirmedSharedAi(thread_id, text);
}

bool ConversationsFacade::NeedsSharedAiConfirmFor(const std::string& thread_id, const std::string& text) const {
  return hub_.HasRouter() && hub_.Router().NeedsSharedAiConfirmFor(thread_id, text);
}

void ConversationsFacade::MarkSharedAiConfirmed(const std::string& thread_id) {
  hub_.Router().MarkSharedAiConfirmed(thread_id);
}

bool ConversationsFacade::IsInitiationOutboundBlocked(const std::string& peer_identity) {
  if (peer_identity.empty()) {
    return false;
  }
  auto* store = hub_.InitiationBilling();
  if (!store || store->IsOpen(peer_identity)) {
    return false;
  }
  const int64_t offer = InitiationPricing::DefaultOfferForFloor(store->Get(peer_identity).floor_minor);
  return !CanPayAmount(offer);
}

Roe<void> ConversationsFacade::SendChargeRequired(const std::string& peer_identity,
                                              const std::optional<int64_t> floor_minor) {
  return hub_.SendChargeRequired(peer_identity, floor_minor);
}


Roe<PaymentPromise> ConversationsFacade::CreatePaymentPromiseOffer(const PaymentPromiseLifecycle::OfferParams& params) {
  return hub_.CreatePaymentPromiseOffer(params);
}

Roe<PaymentPromise> ConversationsFacade::CreatePaymentPromiseOfferForThread(
    const std::string& thread_id, PaymentPromiseLifecycle::OfferParams params) {
  return hub_.CreatePaymentPromiseOfferForThread(thread_id, std::move(params));
}

Roe<PaymentPromise> ConversationsFacade::AcceptPaymentPromise(const std::string& promise_id) {
  return hub_.AcceptPaymentPromise(promise_id);
}

Roe<PaymentPromise> ConversationsFacade::MarkPaymentPromiseDelivering(const std::string& promise_id) {
  return hub_.MarkPaymentPromiseDelivering(promise_id);
}

Roe<PaymentPromise> ConversationsFacade::RecordPaymentPromiseOutcome(const std::string& promise_id,
                                                                const PaymentPromiseState outcome,
                                                                const std::string& note) {
  return hub_.RecordPaymentPromiseOutcome(promise_id, outcome, note);
}

Roe<void> ConversationsFacade::AvoidPaymentPromiseCounterparty(const std::string& promise_id) {
  return hub_.AvoidPaymentPromiseCounterparty(promise_id);
}

Roe<std::vector<PaymentPromise>> ConversationsFacade::ListPaymentPromises() {
  return hub_.ListPaymentPromises();
}

Roe<std::optional<PaymentPromise>> ConversationsFacade::GetPaymentPromise(const std::string& promise_id) {
  return hub_.GetPaymentPromise(promise_id);
}

Roe<std::vector<PaymentPromise>> ConversationsFacade::ListPendingInboundPaymentPromises() {
  return hub_.ListPendingInboundPaymentPromises();
}

Roe<std::optional<PaymentPromise>> ConversationsFacade::GetPendingInboundPaymentPromise(
    const std::string& promise_id) {
  return hub_.GetPendingInboundPaymentPromise(promise_id);
}

Roe<PaymentPromise> ConversationsFacade::AcceptInboundPaymentPromise(const std::string& promise_id) {
  return hub_.AcceptInboundPaymentPromise(promise_id);
}

Roe<bool> ConversationsFacade::IgnoreInboundPaymentPromise(const std::string& promise_id) {
  return hub_.IgnoreInboundPaymentPromise(promise_id);
}

bool ConversationsFacade::ShouldAvoidPaymentCounterparty(const std::string& other_account_id) {
  return hub_.ShouldAvoidPaymentCounterparty(other_account_id);
}

Roe<ThreadMessage> ConversationsFacade::BuildPaymentPromiseControlMessage(const std::string& thread_id,
                                                                      const PaymentPromiseControlType type,
                                                                      const PaymentPromise& promise,
                                                                      const std::string& body_text) {
  return hub_.BuildPaymentPromiseControlMessage(thread_id, type, promise, body_text);
}

// --- Settings / profile helpers ---------------------------------------------

ProfileIdentityView ConversationsFacade::LoadProfileIdentityView() { return hub_.LoadProfileIdentityView(); }

Roe<void> ConversationsFacade::SaveProfileNickname(const std::string& nickname) {
  return hub_.SaveProfileNickname(nickname);
}

Roe<void> ConversationsFacade::RegisterIdentity(const std::string& nickname) { return hub_.RegisterIdentity(nickname); }

Roe<void> ConversationsFacade::UploadProfileIconFromPath(const std::string& path) {
  return hub_.UploadProfileIconFromPath(path);
}

Roe<void> ConversationsFacade::ClearProfileIcon() { return hub_.ClearProfileIcon(); }

Roe<BlobQuotaRecoveryPlan> ConversationsFacade::PlanRelayQuotaRecovery() { return hub_.PlanRelayQuotaRecovery(); }

Roe<void> ConversationsFacade::FreeOldestRelayBlobSlot() { return hub_.FreeOldestRelayBlobSlot(); }

void ConversationsFacade::DrainPendingAttachmentMedia() { hub_.DrainPendingAttachmentMedia(); }

Roe<void> ConversationsFacade::ClearDownloadedAttachments() { return hub_.ClearDownloadedAttachments(); }

void ConversationsFacade::RequestAttachmentDownload(const std::string& thread_id, const std::string& message_id) {
  hub_.RequestAttachmentDownload(thread_id, message_id);
}

Roe<ThreadMessage> ConversationsFacade::SendAttachmentFromPath(const std::string& thread_id, const std::string& path) {
  return hub_.SendAttachmentFromPath(thread_id, path);
}

void ConversationsFacade::SendAttachmentFromPathAsync(const std::string& thread_id, const std::string& path,
                                                      std::function<void(Roe<ThreadMessage>)> on_done) {
  hub_.SendAttachmentFromPathAsync(thread_id, path, std::move(on_done));
}

void ConversationsFacade::EnsureThreadAttachments(const std::string& thread_id) {
  hub_.Attachments().EnsureThreadQueued(thread_id, hub_.Store());
}

void ConversationsFacade::RetryAttachmentDownload(const std::string& thread_id, const std::string& message_id) {
  hub_.Attachments().RetryDownload(thread_id, message_id, hub_.Store());
}

std::optional<std::string> ConversationsFacade::AttachmentLocalPathForMessage(const std::string& thread_id,
                                                                          const std::string& message_id) {
  auto page = hub_.Store().GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return std::nullopt;
  }
  for (const ThreadMessage& message : *page) {
    if (message.id != message_id || message.content_type != ChatContentType::Attachment) {
      continue;
    }
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
    if (!fields) {
      return std::nullopt;
    }
    if (auto view = hub_.Attachments().EnsureLocalViewPath(thread_id, fields->content_hash, fields->mime,
                                                            fields->filename)) {
      if (!view->empty()) {
        return *view;
      }
    }
    const std::string path =
        AttachmentLocalPath(hub_.ProfileDataDir(), thread_id, fields->content_hash, fields->mime, fields->filename);
    if (path.empty()) {
      return std::nullopt;
    }
    return path;
  }
  return std::nullopt;
}

Roe<ConversationsFacade::StoredAiImage> ConversationsFacade::StoreAiImage(const std::string& thread_id,
                                                                          const std::vector<uint8_t>& bytes,
                                                                          const std::string& mime) {
  auto stored = hub_.Attachments().StoreLocalImage(thread_id, bytes, mime);
  if (!stored) {
    return stored.error();
  }
  return StoredAiImage{.content_hash = std::move(stored->content_hash), .view_path = std::move(stored->view_path)};
}

std::optional<std::string> ConversationsFacade::AiImageViewPath(const std::string& thread_id,
                                                                const std::vector<uint8_t>& content_hash,
                                                                const std::string& mime) {
  auto view = hub_.Attachments().EnsureLocalViewPath(thread_id, content_hash, mime, std::string());
  if (!view || view->empty()) {
    return std::nullopt;
  }
  return *view;
}

bool ConversationsFacade::AttachmentOpenNeedsConfirmForMessage(const std::string& thread_id,
                                                           const std::string& message_id) {
  auto page = hub_.Store().GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return true;
  }
  for (const ThreadMessage& message : *page) {
    if (message.id != message_id || message.content_type != ChatContentType::Attachment) {
      continue;
    }
    if (auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json)) {
      const std::string path = AttachmentLocalPath(hub_.ProfileDataDir(), thread_id, fields->content_hash,
                                                    fields->mime, fields->filename);
      return !AttachmentSafeToAutoOpen(path, fields->mime);
    }
    return true;
  }
  return true;
}

Roe<void> ConversationsFacade::RotateBriefLlmKey() { return hub_.RotateBriefLlmKey(); }

mesh::ReachabilitySnapshot ConversationsFacade::Reachability() { return hub_.Reachability(); }

void ConversationsFacade::RunReachabilityProbe(const bool try_upnp) { hub_.RunReachabilityProbe(try_upnp); }

void ConversationsFacade::TryUpnpPortMapping() { hub_.TryUpnpPortMapping(); }

std::string ConversationsFacade::LastMeshError() { return hub_.LastMeshError(); }

bool ConversationsFacade::IsHelpNetworkEnabled() { return hub_.IsHelpNetworkEnabled(); }

// --- Messaging tools helpers ------------------------------------------------

Roe<std::vector<mesh::DirectoryHit>> ConversationsFacade::SearchPeople(const std::string& query) {
  return SearchDirectoryPeople(hub_, query);
}

Roe<std::vector<Contact>> ConversationsFacade::SearchLocalContacts(const std::string& query) {
  return hub_.Contacts().SearchLocal(query);
}

Roe<LocalIdentity> ConversationsFacade::GetLocalIdentity() { return hub_.Identity().Get(); }

Roe<LocalIdentity> ConversationsFacade::UpdateLocalIdentity(const LocalIdentity& identity) {
  return hub_.Identity().Update(identity);
}

Roe<LocalIdentity> ConversationsFacade::FinishAndPersistRegistration(const std::string& nickname) {
  return pbr::FinishAndPersistRegistration(hub_.Registration(), hub_.Identity(), nickname);
}

Roe<RegistrationResult> ConversationsFacade::UpdateRegisteredNickname(const std::string& nickname) {
  return pbr::UpdateRegisteredNickname(hub_.Registration(), hub_.Identity(), nickname);
}

Roe<void> ConversationsFacade::RevokeRegistration() {
  return pbr::RevokeRegistrationWithIdentity(hub_.Registration(), hub_.Identity());
}

} // namespace pbr
