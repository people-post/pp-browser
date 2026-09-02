#include "feature/messaging/MessagingFacade.h"

#include "base/data/PricingTypes.h"
#include "base/messaging/AttachmentCache.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/InitiationPricing.h"
#include "feature/messaging/RegistrationClientUtil.h"
#include "feature/messaging/LinkDeviceCoordinator.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/messaging/PushDeviceCoordinator.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

MessagingFacade::MessagingFacade(MessagingHub& hub) : hub_(hub) {}

// --- Core / lifecycle -------------------------------------------------------

MessagingView MessagingFacade::Snapshot() { return ProjectMessagingView(hub_); }

bool MessagingFacade::HasRouter() { return hub_.HasRouter(); }

bool MessagingFacade::IsInitialized() { return hub_.IsInitialized(); }

bool MessagingFacade::IsMessagingReady() { return hub_.IsMessagingReady(); }

IThreadStore* MessagingFacade::ThreadStore() { return &hub_.Store(); }

void MessagingFacade::BindAgent(AgentSession& agent) { hub_.BindAgent(agent); }

Roe<bool> MessagingFacade::MaybeAutoRenewRegistration(const bool auto_renew_registration) {
  return pbr::MaybeAutoRenewRegistration(hub_.Registration(), hub_.Identity(), auto_renew_registration);
}

Roe<void> MessagingFacade::SyncPushDevices(const bool show_notifications) {
  return PushDeviceCoordinator::SyncWithPreference(hub_, show_notifications);
}

Roe<std::string> MessagingFacade::ExportLinkDevice() {
  if (!hub_.IsInitialized() || hub_.Secrets() == nullptr || hub_.Secrets()->Vault() == nullptr ||
      hub_.PskStore() == nullptr) {
    return Error("Messaging is not ready");
  }
  return LinkDeviceCoordinator::ExportJson(hub_.Identity(), *hub_.Secrets()->Vault(), *hub_.PskStore(),
                                           util::NowUnixMs());
}

Roe<void> MessagingFacade::ImportLinkDevice(const std::string& bundle_json, const std::string& pin) {
  if (!hub_.IsInitialized() || hub_.Secrets() == nullptr || hub_.Secrets()->Vault() == nullptr ||
      hub_.PskStore() == nullptr) {
    return Error("Messaging is not ready");
  }
  auto imported = LinkDeviceCoordinator::Import(hub_.Identity(), *hub_.Secrets()->Vault(), *hub_.PskStore(),
                                                hub_.Secrets(), bundle_json, pin, util::NowUnixMs());
  if (!imported) {
    return imported.error();
  }
  return {};
}

void MessagingFacade::SuspendMeshColdPeers() { hub_.SuspendMeshColdPeers(); }

// --- Inbox ------------------------------------------------------------------

const std::string& MessagingFacade::ActiveThreadId() { return hub_.Inbox().ActiveThreadId(); }

Roe<Thread> MessagingFacade::GetActiveThread() { return hub_.Inbox().GetActiveThread(); }

Roe<std::vector<Thread>> MessagingFacade::ListThreads() { return hub_.Inbox().ListThreads(); }

Roe<Thread> MessagingFacade::OpenThread(const std::string& thread_id) { return hub_.Inbox().OpenThread(thread_id); }

Roe<void> MessagingFacade::CloseThread(const std::string& thread_id) { return hub_.Inbox().CloseThread(thread_id); }

void MessagingFacade::ClearActiveThread() { hub_.Inbox().ClearActiveThread(); }

Roe<Thread> MessagingFacade::CreateNewAiThread() { return hub_.Inbox().CreateNewAiThread(); }

Roe<void> MessagingFacade::UpdatePreview(const std::string& thread_id, const std::string& preview) {
  return hub_.Inbox().UpdatePreview(thread_id, preview);
}

Roe<void> MessagingFacade::ClearThreadHistory(const std::string& thread_id, const bool forget_memory) {
  return hub_.Inbox().ClearThreadHistory(thread_id, forget_memory);
}

Roe<void> MessagingFacade::ForgetThreadMemory(const std::string& thread_id) {
  return hub_.Inbox().ForgetThreadMemory(thread_id);
}

Roe<void> MessagingFacade::SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title) {
  return hub_.Inbox().SetThreadLocalTitle(thread_id, local_title);
}

Roe<Thread> MessagingFacade::FindOrCreateDirectThread(const std::string& contact_id, const ThreadChannel channel) {
  return hub_.Inbox().FindOrCreateDirectThread(contact_id, channel);
}

void MessagingFacade::NotifyThreadChanged() { hub_.Inbox().NotifyThreadChanged(); }

void MessagingFacade::SetOnThreadChanged(std::function<void()> callback) {
  hub_.Inbox().SetOnThreadChanged(std::move(callback));
}

PeerDisplayLabel MessagingFacade::ResolveThreadLabel(const Thread& thread) {
  return hub_.Inbox().ResolveThreadLabel(thread);
}

std::vector<MessageDisplayRow> MessagingFacade::BuildDisplayRows(const std::string& thread_id,
                                                                 const std::optional<int64_t> oldest_inclusive,
                                                                 const std::optional<int64_t> newest_inclusive) {
  return hub_.Inbox().BuildDisplayRows(thread_id, oldest_inclusive, newest_inclusive);
}

bool MessagingFacade::HasLocalMessagesBefore(const std::string& thread_id, const int64_t before_display_order) {
  return hub_.Inbox().HasLocalMessagesBefore(thread_id, before_display_order);
}

int MessagingFacade::SumUnread() { return hub_.Inbox().SumUnread(); }

// --- Store ------------------------------------------------------------------

Roe<std::optional<Thread>> MessagingFacade::GetThread(const std::string& thread_id) {
  return hub_.Store().GetThread(thread_id);
}

Roe<std::vector<ThreadMessage>> MessagingFacade::GetMessagesPage(const std::string& thread_id,
                                                                const std::optional<int64_t> before,
                                                                const int limit) {
  return hub_.Store().GetMessagesPage(thread_id, before, limit);
}

Roe<ThreadMessage> MessagingFacade::AppendMessage(const ThreadMessage& message) {
  return hub_.Store().AppendMessage(message);
}

Roe<bool> MessagingFacade::UpdateMessage(const ThreadMessage& message) {
  return hub_.Store().UpdateMessage(message);
}

Roe<uint32_t> MessagingFacade::GetChatTargetSessionEpoch(const std::string& thread_id) {
  return hub_.Store().GetChatTargetSessionEpoch(thread_id);
}

Roe<PeerSyncState> MessagingFacade::GetPeerSyncState(const std::string& thread_id, const uint32_t epoch) {
  return hub_.Store().GetPeerSyncState(thread_id, epoch);
}

// --- P2P --------------------------------------------------------------------

void MessagingFacade::MaybeTailSync(const std::string& thread_id) { hub_.MeshMessaging().MaybeTailSync(thread_id); }

Roe<ThreadMessage> MessagingFacade::SendUserMessage(const std::string& thread_id, const std::string& text,
                                                    SendRelayOptions opts) {
  return hub_.MeshMessaging().SendUserMessage(thread_id, text, opts);
}

Roe<ThreadMessage> MessagingFacade::SendReaction(const std::string& thread_id, const std::string& target_message_id,
                                                 const std::string& emoji) {
  return hub_.MeshMessaging().SendReaction(thread_id, target_message_id, emoji);
}

Roe<ThreadMessage> MessagingFacade::ClearReaction(const std::string& thread_id, const std::string& target_message_id,
                                                  const std::string& emoji) {
  return hub_.MeshMessaging().ClearReaction(thread_id, target_message_id, emoji);
}

void MessagingFacade::SetOnMessagesChanged(std::function<void()> callback) {
  hub_.MeshMessaging().SetOnMessagesChanged(std::move(callback));
}

void MessagingFacade::SetOnDeliveryNotice(std::function<void(const std::string&)> callback) {
  hub_.MeshMessaging().SetOnDeliveryNotice(std::move(callback));
}

void MessagingFacade::SetOnBackgroundUnread(std::function<void(std::string, std::string, std::string)> callback) {
  hub_.MeshMessaging().SetOnBackgroundUnread(std::move(callback));
}

void MessagingFacade::SyncInboxFromWake(const bool force) { hub_.MeshMessaging().SyncInboxFromWake(force); }

void MessagingFacade::TailSyncActiveE2eThread() { hub_.MeshMessaging().TailSyncActiveE2eThread(); }

void MessagingFacade::WarmPeerForThread(const std::string& thread_id) { hub_.MeshMessaging().WarmPeerForThread(thread_id); }

ThreadPeerLinkView MessagingFacade::GetThreadPeerLink(const std::string& thread_id) {
  return hub_.MeshMessaging().GetThreadPeerLink(thread_id);
}

void MessagingFacade::RetryPeerDial(const std::string& thread_id) { hub_.MeshMessaging().RetryPeerDial(thread_id); }

void MessagingFacade::ScrollBackfill(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done) {
  hub_.MeshMessaging().ScrollBackfill(thread_id, std::move(on_done));
}

void MessagingFacade::SyncWithPeer(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done) {
  hub_.MeshMessaging().SyncWithPeer(thread_id, std::move(on_done));
}

void MessagingFacade::RetryGapSync(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done) {
  hub_.MeshMessaging().RetryGapSync(thread_id, std::move(on_done));
}

Roe<uint32_t> MessagingFacade::StartNewSecureChat(const std::string& thread_id) {
  return hub_.MeshMessaging().StartNewSecureChat(thread_id);
}

Roe<void> MessagingFacade::PauseIntegrityOnly(const std::string& thread_id) {
  return hub_.MeshMessaging().PauseIntegrityOnly(thread_id);
}

Roe<PskExportView> MessagingFacade::EnsurePskGenerated(const std::string& thread_id) {
  return hub_.MeshMessaging().EnsurePskGenerated(thread_id);
}

Roe<PskExportView> MessagingFacade::GetPskExportView(const std::string& thread_id) {
  return hub_.MeshMessaging().GetPskExportView(thread_id);
}

Roe<PskSessionStatus> MessagingFacade::GetPskStatus(const std::string& thread_id) {
  return hub_.MeshMessaging().GetPskStatus(thread_id);
}

Roe<void> MessagingFacade::ImportPskBundleJson(const std::string& thread_id, const std::string& bundle_json) {
  return hub_.MeshMessaging().ImportPskBundleJson(thread_id, bundle_json);
}

Roe<void> MessagingFacade::ImportPskRawBase64(const std::string& thread_id, const std::string& raw_b64) {
  return hub_.MeshMessaging().ImportPskRawBase64(thread_id, raw_b64);
}

Roe<void> MessagingFacade::MarkPskVerified(const std::string& thread_id) {
  return hub_.MeshMessaging().MarkPskVerified(thread_id);
}

Roe<std::string> MessagingFacade::RotatePskAndExportBundle(const std::string& thread_id) {
  return hub_.MeshMessaging().RotatePskAndExportBundle(thread_id);
}

Roe<void> MessagingFacade::LockPublicThreadToThisDevice(const std::string& thread_id) {
  return hub_.MeshMessaging().LockPublicThreadToThisDevice(thread_id);
}

Roe<PublicKeyScope> MessagingFacade::GetPublicKeyScope(const std::string& thread_id) {
  return hub_.MeshMessaging().GetPublicKeyScope(thread_id);
}

Roe<bool> MessagingFacade::CanLockPublicToThisDevice(const std::string& thread_id) {
  return hub_.MeshMessaging().CanLockPublicToThisDevice(thread_id);
}

void MessagingFacade::SetSupportAccountId(std::string account_id) {
  hub_.MeshMessaging().SetSupportAccountId(std::move(account_id));
}

void MessagingFacade::RegisterContactDirectEndpoints(const Contact& contact) {
  hub_.MeshMessaging().RegisterContactDirectEndpoints(contact);
}

void MessagingFacade::RegisterPeerSigningKey(const std::string& kind, const std::string& value,
                                             const std::string& key_b64, const std::string& source) {
  hub_.MeshMessaging().RegisterPeerSigningKey(kind, value, key_b64, source);
}

void MessagingFacade::RegisterPeerKemKey(const std::string& kind, const std::string& value,
                                         const std::string& key_b64, const std::string& source) {
  hub_.MeshMessaging().RegisterPeerKemKey(kind, value, key_b64, source);
}

Roe<Contact> MessagingFacade::AddContactFromDirectoryHit(const DirectoryHit& hit) {
  return hub_.Contacts().AddFromDirectoryHit(hit);
}

Roe<RelayDeleteResult> MessagingFacade::ClearUndeliveredOlderThan(const int older_than_days) {
  return hub_.MeshMessaging().ClearUndeliveredOlderThan(older_than_days);
}

// --- Groups -----------------------------------------------------------------

Roe<void> MessagingFacade::DismissLocalGroup(const std::string& group_id) {
  return hub_.Groups().DismissLocalGroup(group_id);
}

Roe<bool> MessagingFacade::IsLocalOwner(const std::string& group_id) { return hub_.Groups().IsLocalOwner(group_id); }

Roe<std::vector<GroupRosterMember>> MessagingFacade::ListGroupRoster(const std::string& group_id) {
  return hub_.Groups().ListRoster(group_id);
}

bool MessagingFacade::IsMemberUnreachable(const std::string& group_id, const std::string& member_identity) {
  return hub_.Groups().IsMemberUnreachable(group_id, member_identity);
}

Roe<void> MessagingFacade::LeaveGroup(const std::string& group_id) { return hub_.Groups().LeaveGroup(group_id); }

Roe<void> MessagingFacade::LeaveAsOwner(const std::string& group_id, const std::string& new_owner_identity) {
  return hub_.Groups().LeaveAsOwner(group_id, new_owner_identity);
}

Roe<std::string> MessagingFacade::OwnerIdentity(const std::string& group_id) {
  return hub_.Groups().OwnerIdentity(group_id);
}

bool MessagingFacade::IsOwnerUnreachable(const std::string& group_id) {
  return hub_.Groups().IsOwnerUnreachable(group_id);
}

Roe<void> MessagingFacade::RenameGroupShared(const std::string& group_id, const std::string& title) {
  return hub_.Groups().RenameGroupShared(group_id, title);
}

Roe<void> MessagingFacade::RemoveMemberByIdentity(const std::string& group_id, const std::string& member_identity) {
  return hub_.Groups().RemoveMemberByIdentity(group_id, member_identity);
}

std::vector<std::string> MessagingFacade::ListUnreachableMembers(const std::string& group_id) {
  return hub_.Groups().ListUnreachable(group_id);
}

// --- Identity / contacts / directory ----------------------------------------

std::optional<LocalIdentity> MessagingFacade::GetIdentity() {
  if (auto identity = hub_.Identity().Get()) {
    return *identity;
  }
  return std::nullopt;
}

Roe<std::optional<Contact>> MessagingFacade::FindContactByIdentity(const std::string& identity,
                                                                  const ContactIdKind kind) {
  return hub_.Contacts().FindByIdentity(identity, kind);
}

Roe<std::optional<Contact>> MessagingFacade::GetContact(const std::string& contact_id) {
  return hub_.Contacts().Get(contact_id);
}

std::optional<DirectoryHit> MessagingFacade::GetDirectoryShadow(const std::string& peer_id) {
  return hub_.DirectoryShadows().Get(peer_id);
}

// --- Actions ----------------------------------------------------------------

Roe<std::optional<std::string>> MessagingFacade::DispatchAction(const std::string& payload_json) {
  return hub_.Actions().Dispatch(payload_json);
}

void MessagingFacade::SetOnActionMessage(std::function<void(const std::string& message)> callback) {
  hub_.Actions().SetOnActionMessage(std::move(callback));
}

// --- Router -----------------------------------------------------------------

Roe<void> MessagingFacade::RouteMessage(const std::string& thread_id, const std::string& text,
                                        std::optional<std::string> user_payload) {
  return hub_.Router().Route(thread_id, text, std::move(user_payload));
}

bool MessagingFacade::ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                                       const std::optional<std::string>& user_payload) {
  return hub_.Router().ExpectsAgentWork(thread_id, text, user_payload);
}

void MessagingFacade::SetOnLocalAction(
    std::function<void(const std::string&, const std::optional<std::string>&)> callback) {
  hub_.Router().SetOnLocalAction(std::move(callback));
}

void MessagingFacade::SetSharedAiConfirmCallback(MessageRouter::SharedAiConfirmCallback callback) {
  hub_.Router().SetSharedAiConfirmCallback(std::move(callback));
}

void MessagingFacade::MarkSharedAiConfirmed(const std::string& thread_id) {
  hub_.Router().MarkSharedAiConfirmed(thread_id);
}

bool MessagingFacade::IsInitiationOutboundBlocked(const std::string& peer_identity) {
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

Roe<void> MessagingFacade::SendChargeRequired(const std::string& peer_identity,
                                              const std::optional<int64_t> floor_minor) {
  return hub_.SendChargeRequired(peer_identity, floor_minor);
}

// --- Settings / profile helpers ---------------------------------------------

ProfileIdentityView MessagingFacade::LoadProfileIdentityView() { return hub_.LoadProfileIdentityView(); }

Roe<void> MessagingFacade::SaveProfileNickname(const std::string& nickname) {
  return hub_.SaveProfileNickname(nickname);
}

Roe<void> MessagingFacade::RegisterIdentity(const std::string& nickname) { return hub_.RegisterIdentity(nickname); }

Roe<void> MessagingFacade::UploadProfileIconFromPath(const std::string& path) {
  return hub_.UploadProfileIconFromPath(path);
}

Roe<void> MessagingFacade::ClearProfileIcon() { return hub_.ClearProfileIcon(); }

Roe<BlobQuotaRecoveryPlan> MessagingFacade::PlanRelayQuotaRecovery() { return hub_.PlanRelayQuotaRecovery(); }

Roe<void> MessagingFacade::FreeOldestRelayBlobSlot() { return hub_.FreeOldestRelayBlobSlot(); }

void MessagingFacade::DrainPendingAttachmentMedia() { hub_.DrainPendingAttachmentMedia(); }

Roe<void> MessagingFacade::ClearDownloadedAttachments() { return hub_.ClearDownloadedAttachments(); }

void MessagingFacade::RequestAttachmentDownload(const std::string& thread_id, const std::string& message_id) {
  hub_.RequestAttachmentDownload(thread_id, message_id);
}

Roe<ThreadMessage> MessagingFacade::SendAttachmentFromPath(const std::string& thread_id, const std::string& path) {
  return hub_.SendAttachmentFromPath(thread_id, path);
}

void MessagingFacade::EnsureThreadAttachments(const std::string& thread_id) {
  hub_.Attachments().EnsureThreadQueued(thread_id, hub_.Store());
}

void MessagingFacade::RetryAttachmentDownload(const std::string& thread_id, const std::string& message_id) {
  hub_.Attachments().RetryDownload(thread_id, message_id, hub_.Store());
}

std::optional<std::string> MessagingFacade::AttachmentLocalPathForMessage(const std::string& thread_id,
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

bool MessagingFacade::AttachmentOpenNeedsConfirmForMessage(const std::string& thread_id,
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
      return AttachmentOpenNeedsConfirm(fields->mime);
    }
    return true;
  }
  return true;
}

Roe<void> MessagingFacade::RotateBriefLlmKey() { return hub_.RotateBriefLlmKey(); }

ReachabilitySnapshot MessagingFacade::Reachability() { return hub_.Reachability(); }

void MessagingFacade::RunReachabilityProbe(const bool try_upnp) { hub_.RunReachabilityProbe(try_upnp); }

void MessagingFacade::TryUpnpPortMapping() { hub_.TryUpnpPortMapping(); }

std::string MessagingFacade::LastMeshError() { return hub_.LastMeshError(); }

bool MessagingFacade::IsHelpNetworkEnabled() { return hub_.IsHelpNetworkEnabled(); }

// --- Messaging tools helpers ------------------------------------------------

Roe<std::vector<DirectoryHit>> MessagingFacade::SearchPeople(const std::string& query) {
  return hub_.Directory().SearchPeople(query);
}

Roe<std::vector<Contact>> MessagingFacade::SearchLocalContacts(const std::string& query) {
  return hub_.Contacts().SearchLocal(query);
}

Roe<LocalIdentity> MessagingFacade::GetLocalIdentity() { return hub_.Identity().Get(); }

Roe<LocalIdentity> MessagingFacade::UpdateLocalIdentity(const LocalIdentity& identity) {
  return hub_.Identity().Update(identity);
}

Roe<LocalIdentity> MessagingFacade::FinishAndPersistRegistration(const std::string& nickname) {
  return pbr::FinishAndPersistRegistration(hub_.Registration(), hub_.Identity(), nickname);
}

Roe<RegistrationResult> MessagingFacade::UpdateRegisteredNickname(const std::string& nickname) {
  return pbr::UpdateRegisteredNickname(hub_.Registration(), hub_.Identity(), nickname);
}

} // namespace pbr
