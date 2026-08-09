#include "feature/messaging/MessagingChatPorts.h"

#include "base/data/PricingTypes.h"
#include "base/messaging/InitiationPricing.h"
#include "base/net/RegistrationClientUtil.h"
#include "feature/messaging/PushDeviceCoordinator.h"

namespace pbr {

MessagingChatPorts MakeMessagingChatPorts(MessagingHub& hub) {
  MessagingChatPorts ports;
  ports.snapshot = [&hub]() { return ProjectMessagingView(hub); };
  ports.has_router = [&hub]() { return hub.HasRouter(); };
  ports.thread_store = [&hub]() -> IThreadStore* { return &hub.Store(); };
  ports.bind_agent = [&hub](AgentSession& agent) { hub.BindAgent(agent); };
  ports.maybe_auto_renew_registration = [&hub](const bool auto_renew) {
    return MaybeAutoRenewRegistration(hub.Registration(), hub.Identity(), auto_renew);
  };
  ports.sync_push_devices = [&hub](const bool show_notifications) {
    return PushDeviceCoordinator::SyncWithPreference(hub, show_notifications);
  };
  ports.suspend_libp2p_cold_peers = [&hub]() { hub.SuspendLibp2pColdPeers(); };

  ports.active_thread_id = [&hub]() -> const std::string& { return hub.Inbox().ActiveThreadId(); };
  ports.get_active_thread = [&hub]() { return hub.Inbox().GetActiveThread(); };
  ports.list_threads = [&hub]() { return hub.Inbox().ListThreads(); };
  ports.open_thread = [&hub](const std::string& thread_id) { return hub.Inbox().OpenThread(thread_id); };
  ports.close_thread = [&hub](const std::string& thread_id) { return hub.Inbox().CloseThread(thread_id); };
  ports.clear_active_thread = [&hub]() { hub.Inbox().ClearActiveThread(); };
  ports.create_new_ai_thread = [&hub]() { return hub.Inbox().CreateNewAiThread(); };
  ports.update_preview = [&hub](const std::string& thread_id, const std::string& preview) {
    return hub.Inbox().UpdatePreview(thread_id, preview);
  };
  ports.clear_thread_history = [&hub](const std::string& thread_id, const bool forget_memory) {
    return hub.Inbox().ClearThreadHistory(thread_id, forget_memory);
  };
  ports.forget_thread_memory = [&hub](const std::string& thread_id) {
    return hub.Inbox().ForgetThreadMemory(thread_id);
  };
  ports.set_thread_local_title = [&hub](const std::string& thread_id, const std::string& local_title) {
    return hub.Inbox().SetThreadLocalTitle(thread_id, local_title);
  };
  ports.find_or_create_direct_thread = [&hub](const std::string& contact_id, const ThreadChannel channel) {
    return hub.Inbox().FindOrCreateDirectThread(contact_id, channel);
  };
  ports.notify_thread_changed = [&hub]() { hub.Inbox().NotifyThreadChanged(); };
  ports.set_on_thread_changed = [&hub](std::function<void()> callback) {
    hub.Inbox().SetOnThreadChanged(std::move(callback));
  };
  ports.resolve_thread_label = [&hub](const Thread& thread) { return hub.Inbox().ResolveThreadLabel(thread); };
  ports.build_display_rows = [&hub](const std::string& thread_id, const std::optional<int64_t> oldest_inclusive) {
    return hub.Inbox().BuildDisplayRows(thread_id, oldest_inclusive);
  };
  ports.has_local_messages_before = [&hub](const std::string& thread_id, const int64_t before_display_order) {
    return hub.Inbox().HasLocalMessagesBefore(thread_id, before_display_order);
  };

  ports.get_thread = [&hub](const std::string& thread_id) { return hub.Store().GetThread(thread_id); };
  ports.get_messages_page = [&hub](const std::string& thread_id, const std::optional<int64_t> before, const int limit) {
    return hub.Store().GetMessagesPage(thread_id, before, limit);
  };
  ports.append_message = [&hub](const ThreadMessage& message) { return hub.Store().AppendMessage(message); };
  ports.update_message = [&hub](const ThreadMessage& message) { return hub.Store().UpdateMessage(message); };
  ports.get_chat_target_session_epoch = [&hub](const std::string& thread_id) {
    return hub.Store().GetChatTargetSessionEpoch(thread_id);
  };
  ports.get_peer_sync_state = [&hub](const std::string& thread_id, const uint32_t epoch) {
    return hub.Store().GetPeerSyncState(thread_id, epoch);
  };

  ports.maybe_tail_sync = [&hub](const std::string& thread_id) { hub.P2p().MaybeTailSync(thread_id); };
  ports.send_user_message = [&hub](const std::string& thread_id, const std::string& text, SendRelayOptions opts) {
    return hub.P2p().SendUserMessage(thread_id, text, opts);
  };
  ports.set_on_messages_changed = [&hub](std::function<void()> callback) {
    hub.P2p().SetOnMessagesChanged(std::move(callback));
  };
  ports.set_on_delivery_notice = [&hub](std::function<void(const std::string&)> callback) {
    hub.P2p().SetOnDeliveryNotice(std::move(callback));
  };
  ports.set_on_background_unread = [&hub](
                                        std::function<void(std::string, std::string, std::string)> callback) {
    hub.P2p().SetOnBackgroundUnread(std::move(callback));
  };
  ports.sync_inbox_from_wake = [&hub](const bool force) { hub.P2p().SyncInboxFromWake(force); };
  ports.tail_sync_active_e2e_thread = [&hub]() { hub.P2p().TailSyncActiveE2eThread(); };
  ports.warm_peer_for_thread = [&hub](const std::string& thread_id) { hub.P2p().WarmPeerForThread(thread_id); };
  ports.get_thread_peer_link = [&hub](const std::string& thread_id) { return hub.P2p().GetThreadPeerLink(thread_id); };
  ports.retry_peer_dial = [&hub](const std::string& thread_id) { hub.P2p().RetryPeerDial(thread_id); };
  ports.scroll_backfill = [&hub](const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done) {
    hub.P2p().ScrollBackfill(thread_id, std::move(on_done));
  };
  ports.sync_with_peer = [&hub](const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done) {
    hub.P2p().SyncWithPeer(thread_id, std::move(on_done));
  };
  ports.retry_gap_sync = [&hub](const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done) {
    hub.P2p().RetryGapSync(thread_id, std::move(on_done));
  };
  ports.start_new_secure_chat = [&hub](const std::string& thread_id) { return hub.P2p().StartNewSecureChat(thread_id); };
  ports.pause_integrity_only = [&hub](const std::string& thread_id) { return hub.P2p().PauseIntegrityOnly(thread_id); };
  ports.ensure_psk_generated = [&hub](const std::string& thread_id) { return hub.P2p().EnsurePskGenerated(thread_id); };
  ports.get_psk_export_view = [&hub](const std::string& thread_id) { return hub.P2p().GetPskExportView(thread_id); };
  ports.get_psk_status = [&hub](const std::string& thread_id) { return hub.P2p().GetPskStatus(thread_id); };
  ports.import_psk_bundle_json = [&hub](const std::string& thread_id, const std::string& bundle_json) {
    return hub.P2p().ImportPskBundleJson(thread_id, bundle_json);
  };
  ports.import_psk_raw_base64 = [&hub](const std::string& thread_id, const std::string& raw_b64) {
    return hub.P2p().ImportPskRawBase64(thread_id, raw_b64);
  };
  ports.mark_psk_verified = [&hub](const std::string& thread_id) { return hub.P2p().MarkPskVerified(thread_id); };
  ports.rotate_psk_and_export_bundle = [&hub](const std::string& thread_id) {
    return hub.P2p().RotatePskAndExportBundle(thread_id);
  };
  ports.register_contact_direct_endpoints = [&hub](const Contact& contact) {
    hub.P2p().RegisterContactDirectEndpoints(contact);
  };
  ports.register_peer_signing_key = [&hub](const std::string& kind, const std::string& value, const std::string& key_b64,
                                           const std::string& source) {
    hub.P2p().RegisterPeerSigningKey(kind, value, key_b64, source);
  };
  ports.register_peer_kem_key = [&hub](const std::string& kind, const std::string& value, const std::string& key_b64,
                                       const std::string& source) {
    hub.P2p().RegisterPeerKemKey(kind, value, key_b64, source);
  };
  ports.add_contact_from_directory_hit = [&hub](const DirectoryHit& hit) { return hub.Contacts().AddFromDirectoryHit(hit); };

  ports.dismiss_local_group = [&hub](const std::string& group_id) { return hub.Groups().DismissLocalGroup(group_id); };
  ports.is_local_owner = [&hub](const std::string& group_id) { return hub.Groups().IsLocalOwner(group_id); };
  ports.list_group_roster = [&hub](const std::string& group_id) { return hub.Groups().ListRoster(group_id); };
  ports.is_member_unreachable = [&hub](const std::string& group_id, const std::string& member_identity) {
    return hub.Groups().IsMemberUnreachable(group_id, member_identity);
  };
  ports.leave_group = [&hub](const std::string& group_id) { return hub.Groups().LeaveGroup(group_id); };
  ports.leave_as_owner = [&hub](const std::string& group_id, const std::string& new_owner_identity) {
    return hub.Groups().LeaveAsOwner(group_id, new_owner_identity);
  };
  ports.owner_identity = [&hub](const std::string& group_id) { return hub.Groups().OwnerIdentity(group_id); };
  ports.is_owner_unreachable = [&hub](const std::string& group_id) { return hub.Groups().IsOwnerUnreachable(group_id); };
  ports.rename_group_shared = [&hub](const std::string& group_id, const std::string& title) {
    return hub.Groups().RenameGroupShared(group_id, title);
  };
  ports.remove_member_by_identity = [&hub](const std::string& group_id, const std::string& member_identity) {
    return hub.Groups().RemoveMemberByIdentity(group_id, member_identity);
  };
  ports.list_unreachable_members = [&hub](const std::string& group_id) { return hub.Groups().ListUnreachable(group_id); };

  ports.get_identity = [&hub]() -> std::optional<LocalIdentity> {
    if (auto identity = hub.Identity().Get()) {
      return *identity;
    }
    return std::nullopt;
  };
  ports.find_contact_by_identity = [&hub](const std::string& identity, const ContactIdKind kind) {
    return hub.Contacts().FindByIdentity(identity, kind);
  };
  ports.get_contact = [&hub](const std::string& contact_id) { return hub.Contacts().Get(contact_id); };
  ports.get_directory_shadow = [&hub](const std::string& relay_user_id) {
    return hub.DirectoryShadows().Get(relay_user_id);
  };

  ports.dispatch_action = [&hub](const std::string& payload_json) { return hub.Actions().Dispatch(payload_json); };
  ports.set_on_action_message = [&hub](std::function<void(const std::string& message)> callback) {
    hub.Actions().SetOnActionMessage(std::move(callback));
  };

  ports.route_message = [&hub](const std::string& thread_id, const std::string& text,
                               std::optional<std::string> user_payload) {
    return hub.Router().Route(thread_id, text, std::move(user_payload));
  };
  ports.initiation_outbound_blocked = [&hub](const std::string& peer_identity) {
    if (peer_identity.empty()) {
      return false;
    }
    auto* store = hub.InitiationBilling();
    if (!store || store->IsOpen(peer_identity)) {
      return false;
    }
    const int64_t offer = InitiationPricing::DefaultOfferForFloor(store->Get(peer_identity).floor_minor);
    return !CanPayAmount(offer);
  };
  ports.send_charge_required = [&hub](const std::string& peer_identity, std::optional<int64_t> floor_minor) {
    return hub.SendChargeRequired(peer_identity, floor_minor);
  };
  ports.expects_agent_work = [&hub](const std::string& thread_id, const std::string& text,
                                    const std::optional<std::string>& user_payload) {
    return hub.Router().ExpectsAgentWork(thread_id, text, user_payload);
  };
  ports.set_on_local_action = [&hub](
                                  std::function<void(const std::string&, const std::optional<std::string>&)> callback) {
    hub.Router().SetOnLocalAction(std::move(callback));
  };
  ports.set_shared_ai_confirm_callback = [&hub](MessageRouter::SharedAiConfirmCallback callback) {
    hub.Router().SetSharedAiConfirmCallback(std::move(callback));
  };
  ports.mark_shared_ai_confirmed = [&hub](const std::string& thread_id) {
    hub.Router().MarkSharedAiConfirmed(thread_id);
  };

  return ports;
}

} // namespace pbr
