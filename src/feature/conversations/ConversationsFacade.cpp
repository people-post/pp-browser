#include "feature/conversations/ConversationsFacade.h"

#include "foundation/data/PricingTypes.h"
#include "domain/messaging/AttachmentCache.h"
#include "domain/messaging/ChatPayloadCodec.h"
#include "domain/messaging/InitiationPricing.h"
#include "feature/conversations/RegistrationClientUtil.h"
#include "feature/conversations/LinkDeviceCoordinator.h"
#include "feature/conversations/ConversationsHub.h"
#include "feature/conversations/PushDeviceCoordinator.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

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

Roe<void> ConversationsFacade::SyncPushDevices(const bool show_notifications) {
  return PushDeviceCoordinator::SyncWithPreference(hub_, show_notifications);
}

Roe<std::string> ConversationsFacade::ExportLinkDevice() {
  if (!hub_.IsInitialized() || hub_.Secrets() == nullptr || hub_.Secrets()->Vault() == nullptr ||
      hub_.PskStore() == nullptr) {
    return Error("Messaging is not ready");
  }
  return LinkDeviceCoordinator::ExportJson(hub_.Identity(), *hub_.Secrets()->Vault(), *hub_.PskStore(),
                                           util::NowUnixMs());
}

Roe<void> ConversationsFacade::ImportLinkDevice(const std::string& bundle_json, const std::string& pin) {
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

void ConversationsFacade::SuspendMeshColdPeers() { hub_.SuspendMeshColdPeers(); }

// --- Inbox ------------------------------------------------------------------

const std::string& ConversationsFacade::ActiveThreadId() { return hub_.Inbox().ActiveThreadId(); }

Roe<Thread> ConversationsFacade::GetActiveThread() { return hub_.Inbox().GetActiveThread(); }

Roe<std::vector<Thread>> ConversationsFacade::ListThreads() { return hub_.Inbox().ListThreads(); }

Roe<Thread> ConversationsFacade::OpenThread(const std::string& thread_id) { return hub_.Inbox().OpenThread(thread_id); }

Roe<void> ConversationsFacade::CloseThread(const std::string& thread_id) { return hub_.Inbox().CloseThread(thread_id); }

void ConversationsFacade::ClearActiveThread() { hub_.Inbox().ClearActiveThread(); }

Roe<Thread> ConversationsFacade::CreateNewAiThread() { return hub_.Inbox().CreateNewAiThread(); }

Roe<void> ConversationsFacade::UpdatePreview(const std::string& thread_id, const std::string& preview) {
  return hub_.Inbox().UpdatePreview(thread_id, preview);
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

Roe<Thread> ConversationsFacade::FindOrCreateDirectThread(const std::string& contact_id, const ThreadChannel channel) {
  return hub_.Inbox().FindOrCreateDirectThread(contact_id, channel);
}

void ConversationsFacade::NotifyThreadChanged() { hub_.Inbox().NotifyThreadChanged(); }

void ConversationsFacade::SetOnThreadChanged(std::function<void()> callback) {
  hub_.Inbox().SetOnThreadChanged(std::move(callback));
}

PeerDisplayLabel ConversationsFacade::ResolveThreadLabel(const Thread& thread) {
  return hub_.Inbox().ResolveThreadLabel(thread);
}

std::vector<MessageDisplayRow> ConversationsFacade::BuildDisplayRows(const std::string& thread_id,
                                                                 const std::optional<int64_t> oldest_inclusive,
                                                                 const std::optional<int64_t> newest_inclusive) {
  return hub_.Inbox().BuildDisplayRows(thread_id, oldest_inclusive, newest_inclusive);
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

void ConversationsFacade::SetOnDeliveryNotice(std::function<void(const std::string&)> callback) {
  hub_.MeshMessaging().SetOnDeliveryNotice(std::move(callback));
}

void ConversationsFacade::SetOnBackgroundUnread(std::function<void(std::string, std::string, std::string)> callback) {
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

void ConversationsFacade::RegisterPeerSigningKey(const std::string& kind, const std::string& value,
                                             const std::string& key_b64, const std::string& source) {
  hub_.MeshMessaging().RegisterPeerSigningKey(kind, value, key_b64, source);
}

void ConversationsFacade::RegisterPeerKemKey(const std::string& kind, const std::string& value,
                                         const std::string& key_b64, const std::string& source) {
  hub_.MeshMessaging().RegisterPeerKemKey(kind, value, key_b64, source);
}

Roe<Contact> ConversationsFacade::AddContactFromDirectoryHit(const DirectoryHit& hit) {
  return hub_.Contacts().AddFromDirectoryHit(hit);
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

// --- Identity / contacts / directory ----------------------------------------

std::optional<LocalIdentity> ConversationsFacade::GetIdentity() {
  if (auto identity = hub_.Identity().Get()) {
    return *identity;
  }
  return std::nullopt;
}

Roe<std::optional<Contact>> ConversationsFacade::FindContactByIdentity(const std::string& identity,
                                                                  const ContactIdKind kind) {
  return hub_.Contacts().FindByIdentity(identity, kind);
}

Roe<std::optional<Contact>> ConversationsFacade::GetContact(const std::string& contact_id) {
  return hub_.Contacts().Get(contact_id);
}

std::optional<DirectoryHit> ConversationsFacade::GetDirectoryShadow(const std::string& peer_id) {
  return hub_.DirectoryShadows().Get(peer_id);
}

// --- Actions ----------------------------------------------------------------

Roe<std::optional<std::string>> ConversationsFacade::DispatchAction(const std::string& payload_json) {
  return hub_.Actions().Dispatch(payload_json);
}

void ConversationsFacade::SetOnActionMessage(std::function<void(const std::string& message)> callback) {
  hub_.Actions().SetOnActionMessage(std::move(callback));
}

// --- Router -----------------------------------------------------------------

Roe<void> ConversationsFacade::RouteMessage(const std::string& thread_id, const std::string& text,
                                        std::optional<std::string> user_payload) {
  return hub_.Router().Route(thread_id, text, std::move(user_payload));
}

bool ConversationsFacade::ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                                       const std::optional<std::string>& user_payload) {
  return hub_.Router().ExpectsAgentWork(thread_id, text, user_payload);
}

void ConversationsFacade::SetOnLocalAction(
    std::function<void(const std::string&, const std::optional<std::string>&)> callback) {
  hub_.Router().SetOnLocalAction(std::move(callback));
}

void ConversationsFacade::SetSharedAiConfirmCallback(MessageRouter::SharedAiConfirmCallback callback) {
  hub_.Router().SetSharedAiConfirmCallback(std::move(callback));
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

Roe<PaymentPromise> ConversationsFacade::StagePaymentPromiseControlMessage(const ThreadMessage& message) {
  return hub_.StagePaymentPromiseControlMessage(message);
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
      return AttachmentOpenNeedsConfirm(fields->mime);
    }
    return true;
  }
  return true;
}

Roe<void> ConversationsFacade::RotateBriefLlmKey() { return hub_.RotateBriefLlmKey(); }

ReachabilitySnapshot ConversationsFacade::Reachability() { return hub_.Reachability(); }

void ConversationsFacade::RunReachabilityProbe(const bool try_upnp) { hub_.RunReachabilityProbe(try_upnp); }

void ConversationsFacade::TryUpnpPortMapping() { hub_.TryUpnpPortMapping(); }

std::string ConversationsFacade::LastMeshError() { return hub_.LastMeshError(); }

bool ConversationsFacade::IsHelpNetworkEnabled() { return hub_.IsHelpNetworkEnabled(); }

// --- Messaging tools helpers ------------------------------------------------

Roe<std::vector<DirectoryHit>> ConversationsFacade::SearchPeople(const std::string& query) {
  return hub_.Directory().SearchPeople(query);
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


// --- Peer-scoped live announce (Spine C) ------------------------------------

Roe<AnnounceLiveJoinPlan> ConversationsFacade::PlanLiveJoinFromAnnounceTip(const PeerAnnounceTip& tip) {
  return hub_.MeshMessaging().PlanLiveJoinFromAnnounceTip(tip);
}

Roe<AnnounceLiveJoinPlan> ConversationsFacade::PlanLiveJoinFromStoredAnnounce(const std::string& peer_id,
                                                                              const std::string& topic_id,
                                                                              const std::string& program_id) {
  return hub_.MeshMessaging().PlanLiveJoinFromStoredAnnounce(peer_id, topic_id, program_id);
}

Roe<PendingCallInvite> ConversationsFacade::ArmLiveJoinFromAnnounceTip(const PeerAnnounceTip& tip) {
  auto plan = PlanLiveJoinFromAnnounceTip(tip);
  if (!plan) {
    return plan.error();
  }
  auto* calls = hub_.Calls();
  if (!calls) {
    return Error("Call session manager unavailable");
  }
  return calls->Broadcast().ArmJoinFromLiveAnnounce(*plan);
}

Roe<PendingCallInvite> ConversationsFacade::ArmLiveJoinFromStoredAnnounce(const std::string& peer_id,
                                                                          const std::string& topic_id,
                                                                          const std::string& program_id) {
  auto plan = PlanLiveJoinFromStoredAnnounce(peer_id, topic_id, program_id);
  if (!plan) {
    return plan.error();
  }
  auto* calls = hub_.Calls();
  if (!calls) {
    return Error("Call session manager unavailable");
  }
  return calls->Broadcast().ArmJoinFromLiveAnnounce(*plan);
}

Roe<void> ConversationsFacade::AcceptLiveAnnounceJoin(const std::string& call_id) {
  auto* calls = hub_.Calls();
  if (!calls) {
    return Error("Call session manager unavailable");
  }
  return calls->Broadcast().AcceptLiveAnnounceJoin(call_id);
}

Roe<PendingCallInvite> ConversationsFacade::JoinLiveAnnounceFromTip(const PeerAnnounceTip& tip) {
  auto armed = ArmLiveJoinFromAnnounceTip(tip);
  if (!armed) {
    return armed.error();
  }
  if (auto accepted = AcceptLiveAnnounceJoin(armed->call_id); !accepted) {
    return accepted.error();
  }
  return armed;
}

} // namespace pbr
