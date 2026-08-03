#pragma once

#include "base/messaging/AtAiParser.h"
#include "base/messaging/GroupTypes.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/SendRelayOptions.h"
#include "base/messaging/SyncStateTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "base/net/ServiceClients.h"
#include "base/people/ContactTypes.h"
#include "base/people/IdentityTypes.h"
#include "base/ui/ChatWidgetTypes.h"
#include "common/Error.h"
#include "feature/messaging/ChatSyncService.h"
#include "feature/messaging/MessageRouter.h"
#include "feature/messaging/MessagingUiPorts.h"
#include "feature/messaging/P2pMessagingService.h"
#include "feature/messaging/PeerDisplayResolver.h"
#include "feature/messaging/PskSessionCoordinator.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

class AgentSession;

struct ToolRegistry;

/**
 * Chat / inbox / P2P / group / router ports for ChatController and sub-presenters.
 * Application fills from MessagingHub. Clear via BindChatPorts({}).
 */
struct MessagingChatPorts {
  std::function<MessagingView()> snapshot;
  std::function<bool()> has_router;
  std::function<IThreadStore*()> thread_store;
  std::function<void(AgentSession& agent)> bind_agent;
  std::function<void(ToolRegistry& tools)> register_messaging_tools;
  std::function<Roe<bool>(bool auto_renew_registration)> maybe_auto_renew_registration;
  std::function<Roe<void>(bool show_notifications)> sync_push_devices;
  std::function<void()> suspend_libp2p_cold_peers;

  std::function<const std::string&()> active_thread_id;
  std::function<Roe<Thread>()> get_active_thread;
  std::function<Roe<std::vector<Thread>>()> list_threads;
  std::function<Roe<Thread>(const std::string& thread_id)> open_thread;
  std::function<Roe<void>(const std::string& thread_id)> close_thread;
  std::function<void()> clear_active_thread;
  std::function<Roe<Thread>()> create_new_ai_thread;
  std::function<Roe<void>(const std::string& thread_id, const std::string& preview)> update_preview;
  std::function<Roe<void>(const std::string& thread_id, bool forget_memory)> clear_thread_history;
  std::function<Roe<void>(const std::string& thread_id)> forget_thread_memory;
  std::function<Roe<void>(const std::string& thread_id, const std::string& local_title)> set_thread_local_title;
  std::function<Roe<Thread>(const std::string& contact_id, ThreadChannel channel)> find_or_create_direct_thread;
  std::function<void()> notify_thread_changed;
  std::function<void(std::function<void()>)> set_on_thread_changed;
  std::function<PeerDisplayLabel(const Thread& thread)> resolve_thread_label;
  std::function<std::vector<MessageDisplayRow>(const std::string& thread_id, std::optional<int64_t> oldest_inclusive)>
      build_display_rows;
  std::function<bool(const std::string& thread_id, int64_t before_display_order)> has_local_messages_before;

  std::function<Roe<std::optional<Thread>>(const std::string& thread_id)> get_thread;
  std::function<Roe<std::vector<ThreadMessage>>(const std::string& thread_id, std::optional<int64_t> before,
                                                 int limit)>
      get_messages_page;
  std::function<Roe<ThreadMessage>(const ThreadMessage& message)> append_message;
  std::function<Roe<bool>(const ThreadMessage& message)> update_message;
  std::function<Roe<uint32_t>(const std::string& thread_id)> get_chat_target_session_epoch;
  std::function<Roe<PeerSyncState>(const std::string& thread_id, uint32_t epoch)> get_peer_sync_state;

  std::function<void(const std::string& thread_id)> maybe_tail_sync;
  std::function<Roe<ThreadMessage>(const std::string& thread_id, const std::string& text, SendRelayOptions opts)>
      send_user_message;
  std::function<void(std::function<void()>)> set_on_messages_changed;
  std::function<void(std::function<void(const std::string&)>)> set_on_delivery_notice;
  std::function<void(std::function<void(std::string, std::string, std::string)>)> set_on_background_unread;
  std::function<void(bool force)> sync_inbox_from_wake;
  std::function<void()> tail_sync_active_e2e_thread;
  std::function<void(const std::string& thread_id)> warm_peer_for_thread;
  std::function<ThreadPeerLinkView(const std::string& thread_id)> get_thread_peer_link;
  std::function<void(const std::string& thread_id)> retry_peer_dial;
  std::function<void(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done)> scroll_backfill;
  std::function<void(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done)> sync_with_peer;
  std::function<void(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done)> retry_gap_sync;
  std::function<Roe<uint32_t>(const std::string& thread_id)> start_new_secure_chat;
  std::function<Roe<void>(const std::string& thread_id)> pause_integrity_only;
  std::function<Roe<PskExportView>(const std::string& thread_id)> ensure_psk_generated;
  std::function<Roe<PskExportView>(const std::string& thread_id)> get_psk_export_view;
  std::function<Roe<PskSessionStatus>(const std::string& thread_id)> get_psk_status;
  std::function<Roe<void>(const std::string& thread_id, const std::string& bundle_json)> import_psk_bundle_json;
  std::function<Roe<void>(const std::string& thread_id, const std::string& raw_b64)> import_psk_raw_base64;
  std::function<Roe<void>(const std::string& thread_id)> mark_psk_verified;
  std::function<Roe<std::string>(const std::string& thread_id)> rotate_psk_and_export_bundle;
  std::function<void(const Contact& contact)> register_contact_direct_endpoints;
  std::function<void(const std::string& kind, const std::string& value, const std::string& key_b64,
                     const std::string& source)>
      register_peer_signing_key;
  std::function<void(const std::string& kind, const std::string& value, const std::string& key_b64,
                     const std::string& source)>
      register_peer_kem_key;
  std::function<Roe<Contact>(const DirectoryHit& hit)> add_contact_from_directory_hit;

  std::function<Roe<void>(const std::string& group_id)> dismiss_local_group;
  std::function<Roe<bool>(const std::string& group_id)> is_local_owner;
  std::function<Roe<std::vector<GroupRosterMember>>(const std::string& group_id)> list_group_roster;
  std::function<bool(const std::string& group_id, const std::string& member_identity)> is_member_unreachable;
  std::function<Roe<void>(const std::string& group_id)> leave_group;
  std::function<Roe<void>(const std::string& group_id, const std::string& new_owner_identity)> leave_as_owner;
  std::function<Roe<std::string>(const std::string& group_id)> owner_identity;
  std::function<bool(const std::string& group_id)> is_owner_unreachable;
  std::function<Roe<void>(const std::string& group_id, const std::string& title)> rename_group_shared;
  std::function<Roe<void>(const std::string& group_id, const std::string& member_identity)> remove_member_by_identity;
  std::function<std::vector<std::string>(const std::string& group_id)> list_unreachable_members;

  std::function<std::optional<LocalIdentity>()> get_identity;
  std::function<Roe<std::optional<Contact>>(const std::string& identity, ContactIdKind kind)> find_contact_by_identity;
  std::function<Roe<std::optional<Contact>>(const std::string& contact_id)> get_contact;
  std::function<std::optional<DirectoryHit>(const std::string& peer_id)> get_directory_shadow;

  std::function<Roe<std::optional<std::string>>(const std::string& payload_json)> dispatch_action;
  std::function<void(std::function<void(const std::string& message)>)> set_on_action_message;

  std::function<Roe<void>(const std::string& thread_id, const std::string& text,
                          std::optional<std::string> user_payload)>
      route_message;
  std::function<bool(const std::string& thread_id, const std::string& text,
                     const std::optional<std::string>& user_payload)>
      expects_agent_work;
  std::function<void(std::function<void(const std::string&, const std::optional<std::string>&)>)> set_on_local_action;
  std::function<void(MessageRouter::SharedAiConfirmCallback callback)> set_shared_ai_confirm_callback;
  std::function<void(const std::string& thread_id)> mark_shared_ai_confirmed;
};

class MessagingHub;

MessagingChatPorts MakeMessagingChatPorts(MessagingHub& hub);

} // namespace pbr
