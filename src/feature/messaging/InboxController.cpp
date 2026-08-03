#include "feature/messaging/InboxController.h"

#include "feature/messaging/GroupMembershipService.h"
#include "base/ai/StructuredTextParser.h"
#include "base/i18n/LocalizationService.h"
#include "base/messaging/CallControlCodec.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/GroupMembershipCodec.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/ui/ChatFormHelper.h"
#include "common/Utilities.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#include <unordered_map>

namespace pbr {

namespace {

std::string InlineChatActionButtonsRml(const std::vector<TranscriptChatAction>& chat_actions) {
  std::ostringstream out;
  for (size_t i = 0; i < chat_actions.size(); ++i) {
    out << "<button class=\"chat-suggestion\" data-event-click=\"send_chat_action('__ENTRY__', " << i << ")\">"
        << StructuredTextParser::EscapeText(chat_actions[i].label) << "</button>";
  }
  return out.str();
}

std::string HydrateChatActions(const std::string& body_rml, const std::vector<TranscriptChatAction>& chat_actions) {
  if (chat_actions.empty() || body_rml.find("chat-suggestion") != std::string::npos) {
    return body_rml;
  }
  return body_rml + InlineChatActionButtonsRml(chat_actions);
}

std::string SystemLineRml(const std::string& text) {
  return "<div class=\"chat-system-line muted\"><p>" + StructuredTextParser::EscapeText(text) + "</p></div>";
}

bool IsPlumbingCallControl(const CallControlType type) {
  switch (type) {
  case CallControlType::CallMediaKey:
  case CallControlType::CallSdp:
  case CallControlType::CallIce:
  case CallControlType::CallSfuAttach:
    return true;
  default:
    return false;
  }
}

std::optional<std::string> CallDetailJson(const ThreadMessage& message) {
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("detail") || !payload["detail"].is_string()) {
    return std::nullopt;
  }
  return payload["detail"].get<std::string>();
}

std::string FormatCallDurationMs(const int64_t duration_ms) {
  if (duration_ms < 0) {
    return {};
  }
  const int64_t total_sec = duration_ms / 1000;
  const int64_t hours = total_sec / 3600;
  const int64_t mins = (total_sec % 3600) / 60;
  const int64_t secs = total_sec % 60;
  char buf[32];
  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", static_cast<long long>(hours),
                  static_cast<long long>(mins), static_cast<long long>(secs));
  } else {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(mins),
                  static_cast<long long>(secs));
  }
  return buf;
}

} // namespace

InboxController::InboxController(IThreadStore& store, ContactsStore& contacts, PeerDisplayResolver& labels,
                                 DirectoryShadowCache* shadows)
    : store_(store), contacts_(contacts), labels_(labels), shadows_(shadows) {
  redirectLogger("InboxController");
}

void InboxController::SetGroupMembership(GroupMembershipService* groups) {
  groups_ = groups;
}

Roe<std::vector<Thread>> InboxController::ListThreads() {
  return store_.ListThreads();
}

Roe<Thread> InboxController::GetActiveThread() const {
  if (active_thread_id_.empty()) {
    return Error("No active thread");
  }
  auto thread = store_.GetThread(active_thread_id_);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Active thread not found");
  }
  return **thread;
}

Roe<Thread> InboxController::OpenThread(const std::string& thread_id) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  active_thread_id_ = thread_id;
  MarkThreadRead(thread_id);
  if (on_thread_changed_) {
    on_thread_changed_();
  }
  return **thread;
}

void InboxController::ClearActiveThread() {
  if (active_thread_id_.empty()) {
    return;
  }
  active_thread_id_.clear();
  if (on_thread_changed_) {
    on_thread_changed_();
  }
}

Roe<Thread> InboxController::CreateNewAiThread() {
  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Ai;
  thread.title = "New chat";
  thread.preview = "";
  thread.updated_at = util::NowUnixMs();

  auto saved = store_.UpsertThread(thread);
  if (!saved) {
    return saved.error();
  }
  return OpenThread(saved->id);
}

Roe<void> InboxController::CloseThread(const std::string& thread_id) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  if ((*thread)->kind == ThreadKind::Group && (*thread)->group_id && groups_) {
    // Always clear local membership before deleting the thread so inbound traffic cannot resurrect it.
    (void)groups_->DismissLocalGroup(*(*thread)->group_id);
  }

  const bool was_active = active_thread_id_ == thread_id;
  auto deleted = store_.DeleteThread(thread_id);
  if (!deleted || !*deleted) {
    return Error("Failed to delete thread");
  }

  if (was_active) {
    active_thread_id_.clear();
    auto threads = store_.ListThreads();
    if (threads) {
      for (const Thread& candidate : *threads) {
        if (candidate.id == thread_id) {
          continue;
        }
        if (auto opened = OpenThread(candidate.id)) {
          return {};
        }
      }
    }
    if (on_thread_changed_) {
      on_thread_changed_();
    }
  } else if (on_thread_changed_) {
    on_thread_changed_();
  }
  return {};
}

Roe<void> InboxController::ClearThreadHistory(const std::string& thread_id, const bool forget_memory) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  if (auto cleared = store_.ClearMessages(thread_id, ClearMessagesOptions{.forget_memory = forget_memory}); !cleared) {
    return cleared.error();
  }

  Thread updated = **thread;
  updated.preview = "";
  updated.unread_count = 0;
  updated.updated_at = util::NowUnixMs();
  if (!store_.UpsertThread(updated)) {
    return Error("Failed to update thread after clear");
  }

  if (on_thread_changed_) {
    on_thread_changed_();
  }
  return {};
}

Roe<void> InboxController::ForgetThreadMemory(const std::string& thread_id) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind != ThreadKind::Ai) {
    return Error("Forget memory is only available for AI threads");
  }

  if (auto cleared = store_.ClearThreadMemory(thread_id); !cleared) {
    return cleared.error();
  }

  if (on_thread_changed_) {
    on_thread_changed_();
  }
  return {};
}

Roe<Thread> InboxController::FindOrCreateDirectThread(const std::string& contact_id) {
  return FindOrCreateDirectThread(contact_id, ThreadChannel::E2ePublic);
}

Roe<Thread> InboxController::FindOrCreateDirectThread(const std::string& contact_id, ThreadChannel channel) {
  auto contact = contacts_.Get(contact_id);
  if (!contact) {
    return contact.error();
  }
  if (!*contact) {
    return Error("Contact not found");
  }

  const DirectChatTarget target = DirectChatTargetFromContact(**contact, channel);
  if (target.peer_identity_value.empty()) {
    return Error("Contact has no messaging identity");
  }

  const std::string title =
      (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
  auto thread = store_.FindOrCreateDirectThread(target, contact_id, title);
  if (!thread) {
    return thread.error();
  }
  return OpenThread(thread->id);
}

Roe<Thread> InboxController::CreateGroup(const std::string& title,
                                         const std::vector<std::string>& member_contact_ids) {
  if (!groups_) {
    return Error("Messaging not initialized");
  }
  auto created = groups_->CreateGroup(title, member_contact_ids);
  if (!created) {
    return created.error();
  }
  return OpenThread(created->id);
}

Roe<Thread> InboxController::CreateDirectThread(const std::string& contact_id, ThreadChannel channel) {
  auto contact = contacts_.Get(contact_id);
  if (!contact) {
    return contact.error();
  }
  if (!*contact) {
    return Error("Contact not found");
  }

  const DirectChatTarget target = DirectChatTargetFromContact(**contact, channel);
  if (target.peer_identity_value.empty()) {
    return Error("Contact has no messaging identity");
  }

  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Direct;
  thread.channel = channel;
  thread.peer_identity_kind = target.peer_identity_kind;
  thread.peer_identity_value = target.peer_identity_value;
  thread.title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
  thread.participant_contact_ids = {contact_id};
  thread.preview = "";
  thread.updated_at = util::NowUnixMs();
  thread.encrypted = ThreadChannelIsE2e(channel);

  auto saved = store_.FindOrCreateDirectThread(target, contact_id, thread.title);
  if (!saved) {
    return saved.error();
  }
  return OpenThread(saved->id);
}

void InboxController::MarkThreadRead(const std::string& thread_id) {
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }
  if ((*thread)->unread_count == 0) {
    return;
  }
  Thread updated = **thread;
  updated.unread_count = 0;
  (void)store_.UpsertThread(updated);
}

void InboxController::IncrementUnread(const std::string& thread_id, const int delta) {
  if (delta <= 0 || thread_id.empty() || thread_id == active_thread_id_) {
    return;
  }
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }
  Thread updated = **thread;
  updated.unread_count += delta;
  updated.updated_at = util::NowUnixMs();
  (void)store_.UpsertThread(updated);
}

void InboxController::OnInboundMessagePersisted(const std::string& thread_id,
                                                const std::optional<std::string>& preview) {
  if (thread_id.empty()) {
    return;
  }
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }

  Thread updated = **thread;
  bool changed = false;
  if (preview && *preview != updated.preview) {
    updated.preview = *preview;
    changed = true;
  }
  if (thread_id != active_thread_id_) {
    updated.unread_count += 1;
    changed = true;
  }
  if (!changed) {
    return;
  }
  updated.updated_at = util::NowUnixMs();
  (void)store_.UpsertThread(updated);
}

int InboxController::SumUnread() const {
  auto threads = store_.ListThreads();
  if (!threads) {
    return 0;
  }
  int total = 0;
  for (const Thread& thread : *threads) {
    total += thread.unread_count;
  }
  return total;
}

int InboxController::SumUnreadForContact(const std::string& contact_id) const {
  if (contact_id.empty()) {
    return 0;
  }
  auto threads = store_.ListThreads();
  if (!threads) {
    return 0;
  }
  int total = 0;
  for (const Thread& thread : *threads) {
    if (std::find(thread.participant_contact_ids.begin(), thread.participant_contact_ids.end(), contact_id) ==
        thread.participant_contact_ids.end()) {
      continue;
    }
    total += thread.unread_count;
  }
  return total;
}

Roe<void> InboxController::UpdatePreview(const std::string& thread_id, const std::string& preview) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  Thread updated = **thread;
  updated.preview = preview;
  updated.updated_at = util::NowUnixMs();
  if (store_.UpsertThread(updated)) {
    return {};
  }
  return Error("Failed to update thread preview");
}

void InboxController::SetOnThreadChanged(ThreadChangedCallback callback) {
  on_thread_changed_ = std::move(callback);
}

void InboxController::NotifyThreadChanged() {
  if (on_thread_changed_) {
    on_thread_changed_();
  }
}

PeerDisplayLabel InboxController::ResolveThreadLabel(const Thread& thread) const {
  if (shadows_ && thread.kind == ThreadKind::Direct && !thread.peer_identity_value.empty()) {
    shadows_->EnsureLookup(thread.peer_identity_value);
  }
  return labels_.ResolveThread(thread);
}

Roe<void> InboxController::SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind != ThreadKind::Group) {
    return Error("Local titles are only supported for groups");
  }
  Thread updated = **thread;
  updated.local_title = local_title;
  updated.updated_at = util::NowUnixMs();
  if (auto saved = store_.UpsertThread(updated); !saved) {
    return saved.error();
  }
  NotifyThreadChanged();
  return {};
}

std::string InboxController::ResolveSenderLabel(const std::string& sender_contact_id) const {
  if (shadows_ && sender_contact_id.rfind("relay:", 0) == 0) {
    shadows_->EnsureLookup(sender_contact_id);
  }
  return labels_.ResolveSender(sender_contact_id).title;
}

std::string InboxController::ResolveRowClass(const std::string& sender_contact_id) const {
  if (sender_contact_id == kLocalSelfContactId) {
    return "message-row-user";
  }
  if (sender_contact_id == kAiAssistantContactId) {
    return "message-row-ai";
  }
  return "message-row-peer";
}

std::string InboxController::BuildSharedBadgeHtml(const ThreadMessage& message) const {
  if (!message.ai_invoke_mode ||
      (message.ai_invoke_mode != "shared_reply" && message.ai_invoke_mode != "shared_full")) {
    return "";
  }
  return "<span class=\"chat-shared-badge muted\">Shared</span>";
}

std::string InboxController::FormatCallPeerLabel(const std::string& identity) const {
  if (identity.empty()) {
    return Tr("call.label.someone");
  }
  if (identity == kLocalSelfContactId) {
    return Tr("call.label.you");
  }
  const std::string label = ResolveSenderLabel(identity);
  return label.empty() ? Tr("call.label.someone") : label;
}

std::string InboxController::BuildCallHistoryRml(const ThreadMessage& message,
                                                 const CallControlType type) const {
  if (IsPlumbingCallControl(type)) {
    return {};
  }

  const auto detail_json = CallDetailJson(message);
  const bool outbound = message.sender_contact_id == kLocalSelfContactId;
  std::string line;

  switch (type) {
  case CallControlType::CallStarted: {
    CallMediaMode mode = CallMediaMode::Voice;
    if (detail_json) {
      if (auto started = CallControlCodec::DecodeStarted(*detail_json)) {
        mode = started->media_mode;
      }
    }
    line = mode == CallMediaMode::Video ? Tr("call.history.started_video") : Tr("call.history.started_voice");
    break;
  }
  case CallControlType::CallInvite: {
    CallMediaMode mode = CallMediaMode::Voice;
    std::string peer;
    if (detail_json) {
      if (auto invite = CallControlCodec::DecodeInvite(*detail_json)) {
        mode = invite->media_mode;
        peer = outbound ? invite->invitee_identity : invite->inviter_identity;
      }
    }
    if (peer.empty() && !outbound) {
      peer = message.sender_contact_id;
    }
    const std::string name = FormatCallPeerLabel(peer);
    if (outbound) {
      line = mode == CallMediaMode::Video ? Tr("call.history.outgoing_video", {{"name", name}})
                                          : Tr("call.history.outgoing_voice", {{"name", name}});
    } else {
      line = mode == CallMediaMode::Video ? Tr("call.history.incoming_video", {{"name", name}})
                                          : Tr("call.history.incoming_voice", {{"name", name}});
    }
    break;
  }
  case CallControlType::CallAccept: {
    std::string identity = message.sender_contact_id;
    if (detail_json) {
      if (auto accept = CallControlCodec::DecodeAccept(*detail_json)) {
        if (!accept->identity.empty()) {
          identity = accept->identity;
        }
      }
    }
    const std::string name = outbound ? Tr("call.label.you") : FormatCallPeerLabel(identity);
    line = Tr("call.history.joined", {{"name", name}});
    break;
  }
  case CallControlType::CallDecline: {
    std::string identity = message.sender_contact_id;
    if (detail_json) {
      if (auto decline = CallControlCodec::DecodeDecline(*detail_json)) {
        if (!decline->identity.empty()) {
          identity = decline->identity;
        }
      }
    }
    const std::string name = outbound ? Tr("call.label.you") : FormatCallPeerLabel(identity);
    line = Tr("call.history.declined", {{"name", name}});
    break;
  }
  case CallControlType::CallLeave: {
    std::string identity = message.sender_contact_id;
    if (detail_json) {
      if (auto leave = CallControlCodec::DecodeLeave(*detail_json)) {
        if (!leave->identity.empty()) {
          identity = leave->identity;
        }
      }
    }
    const std::string name = outbound ? Tr("call.label.you") : FormatCallPeerLabel(identity);
    line = Tr("call.history.left", {{"name", name}});
    break;
  }
  case CallControlType::CallEnded: {
    std::optional<int64_t> duration_ms;
    if (detail_json) {
      if (auto ended = CallControlCodec::DecodeEnded(*detail_json)) {
        duration_ms = ended->duration_ms;
      }
    }
    if (duration_ms && *duration_ms >= 0) {
      const std::string duration = FormatCallDurationMs(*duration_ms);
      if (!duration.empty()) {
        line = Tr("call.history.ended_duration", {{"duration", duration}});
        break;
      }
    }
    line = Tr("call.history.ended");
    break;
  }
  case CallControlType::CallRoster: {
    size_t count = 0;
    if (detail_json) {
      if (auto roster = CallControlCodec::DecodeRoster(*detail_json)) {
        for (const CallRosterEntry& entry : roster->participants) {
          if (entry.state == CallParticipantState::Joined || entry.state == CallParticipantState::Ringing ||
              entry.state == CallParticipantState::Invited) {
            ++count;
          }
        }
        if (count == 0) {
          count = roster->participants.size();
        }
      }
    }
    if (count == 0) {
      return {};
    }
    line = Tr("call.history.roster", {{"count", std::to_string(count)}});
    break;
  }
  case CallControlType::CallMediaKey:
  case CallControlType::CallSdp:
  case CallControlType::CallIce:
  case CallControlType::CallSfuAttach:
    return {};
  }

  if (line.empty()) {
    line = message.text.empty() ? Tr("call.history.ended") : message.text;
  }
  return SystemLineRml(line);
}

std::string InboxController::BuildSystemRml(const ThreadMessage& message) const {
  if (const auto call_type = CallControlCodec::ControlTypeFromMessage(message)) {
    return BuildCallHistoryRml(message, *call_type);
  }

  const auto control_type = GroupMembershipCodec::ControlTypeFromMessage(message);
  if (control_type && *control_type == GroupMembershipControlType::GroupInvite) {
    if (GroupMembershipCodec::InviteResolutionFromMessage(message)) {
      return SystemLineRml(message.text);
    }
    auto invite = GroupMembershipCodec::DecodeInviteFromMessage(message);
    const std::string title =
        invite ? invite->group_title : (message.text.empty() ? "Group invitation" : message.text);
    std::string html = "<div class=\"chat-card chat-group-invite\"><h3 class=\"heading-3\">" +
                       StructuredTextParser::EscapeText(title) + "</h3>";
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(message.text) + "</p>";
    html += HydrateChatActions("", message.chat_actions);
    html += "</div>";
    if (html.find("__ENTRY__") != std::string::npos) {
      return InjectEntryPlaceholders(html, message.id);
    }
    return html;
  }
  if (control_type && *control_type == GroupMembershipControlType::GroupOwnerUnreachable) {
    if (GroupMembershipCodec::IsOwnerUnreachableResolved(message)) {
      return SystemLineRml(message.text);
    }
    const std::string body =
        "You can keep chatting with people who are online. Inviting, renaming for everyone, and removing "
        "members need the owner. If they’ve left for good, start a new group from this one.";
    std::string html = "<div class=\"chat-card chat-group-invite\"><h3 class=\"heading-3\">" +
                       StructuredTextParser::EscapeText("Owner hasn’t been reachable") + "</h3>";
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(body) + "</p>";
    html += HydrateChatActions("", message.chat_actions);
    html += "</div>";
    if (html.find("__ENTRY__") != std::string::npos) {
      return InjectEntryPlaceholders(html, message.id);
    }
    return html;
  }
  return SystemLineRml(message.text);
}

std::string InboxController::BuildContactCardRml(const ThreadMessage& message) const {
  auto fields = ChatPayloadCodec::DecodeContactCardJson(message.payload_json);
  const std::string name =
      fields ? fields->display_name : (message.text.empty() ? "Contact" : message.text);
  const std::string relay = fields && !fields->relay_user_id.empty() ? fields->relay_user_id : "";
  std::string html = "<div class=\"chat-card chat-contact-card\"><h3 class=\"heading-3\">" +
                     StructuredTextParser::EscapeText(name) + "</h3>";
  if (!relay.empty()) {
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(relay) + "</p>";
  }
  html += "</div>";
  return html;
}

std::string InboxController::BuildCryptoTxRml(const ThreadMessage& message) const {
  auto fields = ChatPayloadCodec::DecodeCryptoTxJson(message.payload_json);
  const std::string summary = message.text.empty() ? "Transaction" : message.text;
  std::string html = "<div class=\"chat-card chat-tx-card\"><h3 class=\"heading-3\">" +
                     StructuredTextParser::EscapeText(summary) + "</h3>";
  if (fields) {
    html += "<p class=\"text muted\">" + StructuredTextParser::EscapeText(fields->asset) + " · " +
            StructuredTextParser::EscapeText(fields->amount) + " · " +
            StructuredTextParser::EscapeText(fields->direction) + "</p>";
    if (!fields->status.empty()) {
      html += "<p class=\"text muted\">Status: " + StructuredTextParser::EscapeText(fields->status) + "</p>";
    }
  }
  html += "</div>";
  return html;
}

std::string InboxController::BuildMessageRml(const ThreadMessage& message) const {
  if (message.content_rml) {
    if (message.content_rml->find("__ENTRY__") != std::string::npos) {
      return InjectEntryPlaceholders(*message.content_rml, message.id);
    }
    return *message.content_rml;
  }
  if (message.content_type == ChatContentType::System) {
    return BuildSystemRml(message);
  }
  if (message.content_type == ChatContentType::ContactCard) {
    return BuildContactCardRml(message);
  }
  if (message.content_type == ChatContentType::CryptoTx) {
    return BuildCryptoTxRml(message);
  }
  const std::string bubble_class = message.sender_contact_id == kLocalSelfContactId ? "bubble-user" : "bubble-assistant";
  const std::string paragraph =
      message.sender_contact_id == kLocalSelfContactId ? "<p class=\"bubble-text\">" : "<p>";
  std::string badges = BuildSharedBadgeHtml(message);
  if (!badges.empty()) {
    badges = "<div class=\"chat-message-meta\">" + badges + "</div>";
  }
  std::string body = badges + "<div class=\"bubble " + bubble_class + "\" selectable=\"text\">" + paragraph +
                     StructuredTextParser::EscapeText(message.text) + "</p></div>";
  body = HydrateChatActions(body, message.chat_actions);
  if (body.find("__ENTRY__") != std::string::npos) {
    return InjectEntryPlaceholders(body, message.id);
  }
  return body;
}

bool InboxController::HasLocalMessagesBefore(const std::string& thread_id,
                                             int64_t before_display_order) const {
  auto page = store_.GetMessagesPage(thread_id, before_display_order, 1);
  return page && !page->empty();
}

std::vector<MessageDisplayRow> InboxController::BuildDisplayRows(
    const std::string& thread_id, std::optional<int64_t> oldest_inclusive) const {
  std::vector<MessageDisplayRow> rows;
  std::vector<ThreadMessage> messages;
  std::optional<int64_t> before;
  for (;;) {
    auto page = store_.GetMessagesPage(thread_id, before, kDefaultMessagesPageSize);
    if (!page || page->empty()) {
      break;
    }
    const int64_t page_oldest = page->front().display_order;
    const size_t page_size = page->size();
    if (messages.empty()) {
      messages = std::move(*page);
    } else {
      messages.insert(messages.begin(), page->begin(), page->end());
    }
    if (!oldest_inclusive.has_value()) {
      break;
    }
    if (page_oldest <= *oldest_inclusive) {
      messages.erase(std::remove_if(messages.begin(), messages.end(),
                                    [&](const ThreadMessage& m) {
                                      return m.display_order < *oldest_inclusive;
                                    }),
                     messages.end());
      break;
    }
    if (page_size < kDefaultMessagesPageSize) {
      break;
    }
    before = page_oldest;
  }

  std::unordered_map<std::string, size_t> row_index_by_id;
  std::unordered_map<std::string, size_t> annotation_count_by_target;
  std::vector<ThreadMessage> orphan_annotations;
  std::vector<ThreadMessage> pending_annotations;

  for (const ThreadMessage& message : messages) {
    if (message.content_type == ChatContentType::Annotation) {
      pending_annotations.push_back(message);
      continue;
    }
    if (message.content_type != ChatContentType::Text && message.content_type != ChatContentType::System &&
        message.content_type != ChatContentType::ContactCard && message.content_type != ChatContentType::CryptoTx) {
      continue;
    }
    if (const auto call_type = CallControlCodec::ControlTypeFromMessage(message)) {
      if (IsPlumbingCallControl(*call_type)) {
        continue;
      }
    }

    MessageDisplayRow row;
    row.message_id = message.id.c_str();
    row.display_order = message.display_order;
    // Call history lines already include names; skip the sender chip above the system line.
    if (CallControlCodec::IsCallControlMessage(message)) {
      row.sender_label = "";
    } else {
      row.sender_label = ResolveSenderLabel(message.sender_contact_id).c_str();
    }
    row.content_rml = BuildMessageRml(message).c_str();
    if (row.content_rml.empty()) {
      continue;
    }
    row.row_class = ResolveRowClass(message.sender_contact_id).c_str();
    // Local is expected for AI/system-local rows — skip it to keep meta quiet.
    if (!CallControlCodec::IsCallControlMessage(message) && message.transport &&
        *message.transport != MessageTransport::Local) {
      row.transport_badge = MessageTransportBadgeLabel(*message.transport).c_str();
    }
    row.has_content = true;
    row_index_by_id[message.id] = rows.size();
    rows.push_back(std::move(row));
  }

  for (const ThreadMessage& annotation : pending_annotations) {
    const std::string target_id =
        annotation.target_message_id.value_or("");
    if (target_id.empty()) {
      orphan_annotations.push_back(annotation);
      continue;
    }
    const auto row_it = row_index_by_id.find(target_id);
    if (row_it == row_index_by_id.end()) {
      orphan_annotations.push_back(annotation);
      continue;
    }
    if (annotation_count_by_target[target_id] >= kMaxAnnotationsPerTarget) {
      continue;
    }
    ++annotation_count_by_target[target_id];
    const std::string badge =
        "<span class=\"chat-annotation-badge\" title=\"" +
        StructuredTextParser::EscapeText(annotation.text) + "\">" +
        StructuredTextParser::EscapeText(annotation.text) + "</span>";
    rows[row_it->second].content_rml =
        (std::string(rows[row_it->second].content_rml.c_str()) + badge).c_str();
  }

  for (const ThreadMessage& annotation : orphan_annotations) {
    MessageDisplayRow row;
    row.message_id = annotation.id.c_str();
    row.display_order = annotation.display_order;
    row.sender_label = ResolveSenderLabel(annotation.sender_contact_id).c_str();
    row.content_rml =
        ("<div class=\"chat-orphan-annotation muted\"><span class=\"chat-annotation-badge\">" +
         StructuredTextParser::EscapeText(annotation.text) +
         "</span> <span class=\"text-xs\">(unlinked)</span></div>")
            .c_str();
    row.row_class = "message-row-annotation-orphan";
    row.has_content = true;
    rows.push_back(std::move(row));
  }

  return rows;
}

} // namespace pbr
