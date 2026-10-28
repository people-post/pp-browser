#include "feature/conversations/MessageRouter.h"

#include "lib/messaging/AtAiParser.h"
#include "common/PlatformLimits.h"
#include "common/thread/IThreadStore.h"
#include "lib/messaging/SendRelayOptions.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/thread/ThreadTypes.h"
#include "common/PbrCompat.h"

namespace pbr {

MessageRouter::MessageRouter(InboxController& inbox, MeshDeliveryOrchestrator& mesh_messaging,
                             AgentInboundPorts agent, IThreadStore& store)
    : mesh_messaging_(mesh_messaging), agent_(std::move(agent)), store_(store) {
  (void)inbox;
  redirectLogger("MessageRouter");
}

void MessageRouter::SetOnLocalAction(
    std::function<void(const std::string& message, const std::optional<std::string>& payload)> cb) {
  on_local_action_ = std::move(cb);
}

void MessageRouter::SetSharedAiConfirmCallback(SharedAiConfirmCallback callback) {
  shared_ai_confirm_ = std::move(callback);
}

void MessageRouter::MarkSharedAiConfirmed(const std::string& thread_id) {
  shared_ai_confirmed_threads_.insert(thread_id);
}

bool MessageRouter::NeedsSharedAiConfirm(const std::string& thread_id) const {
  return shared_ai_confirmed_threads_.find(thread_id) == shared_ai_confirmed_threads_.end();
}

bool MessageRouter::IsAgentToolPayload(const ThreadKind kind, const std::optional<std::string>& user_payload) {
  // Only an AI thread runs tool payloads; anywhere else the text would be relayed to the peer.
  if (kind != ThreadKind::Ai || !user_payload || user_payload->empty()) {
    return false;
  }
  const auto doc = TryParseObject(*user_payload);
  return doc && !doc->getString("type") && doc->getString("tool").value_or("") != "";
}

std::optional<std::string> MessageRouter::LocalActionTool(const std::optional<std::string>& user_payload) {
  if (!user_payload || user_payload->empty()) {
    return std::nullopt;
  }
  const auto doc = TryParseObject(*user_payload);
  if (!doc || doc->getString("type").value_or("") != "local_action") {
    return std::nullopt;
  }
  return doc->getString("tool").value_or("");
}

bool MessageRouter::IsAgentLocalActionPayload(const std::optional<std::string>& user_payload) {
  const auto tool = LocalActionTool(user_payload);
  return tool && (*tool == "open_conversation" || *tool == "start_conversation" || *tool == "search_people");
}

Roe<void> MessageRouter::ValidateUserPayload(const std::optional<std::string>& user_payload) {
  if (user_payload && user_payload->size() > kMaxUserPayloadBytes) {
    return Error("User payload exceeds limit of " + std::to_string(kMaxUserPayloadBytes) + " bytes");
  }
  return {};
}

Roe<void> MessageRouter::RouteSharedAi(const std::string& thread_id, const std::string& prompt,
                                       const AtAiMode mode) {
  if (!agent_.submit_scoped_assist) {
    return Error("Agent not bound");
  }
  if (AgentBusy()) {
    return Error(kAgentBusyError); // before the "@ai++" question goes to the peer
  }

  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return Error("Thread not found");
  }

  std::optional<std::string> seq_owner;
  if (!(*thread)->participant_contact_ids.empty()) {
    seq_owner = (*thread)->participant_contact_ids.front();
  }

  if (mode == AtAiMode::SharedFull) {
    SendRelayOptions user_opts;
    user_opts.generation = "user";
    user_opts.ai_invoke_mode = "shared_full";
    const bool group = (*thread)->kind == ThreadKind::Group;
    if (!group) {
      user_opts.seq_owner_contact_id = seq_owner;
    }
    auto sent = group ? mesh_messaging_.SendGroupMessage(thread_id, prompt, user_opts)
                      : mesh_messaging_.SendUserMessage(thread_id, prompt, user_opts);
    if (!sent) {
      return sent.error();
    }
  }

  agent_.submit_scoped_assist(thread_id, prompt, {}, mode);
  return {};
}

Roe<void> MessageRouter::Route(const std::string& thread_id, const std::string& text,
                               std::optional<std::string> user_payload) {
  if (text.empty()) {
    return Error("Empty message");
  }
  auto payload_valid = ValidateUserPayload(user_payload);
  if (!payload_valid) {
    return payload_valid.error();
  }

  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  // A button of the on-device app-action flow, in an AI thread or (after "@ai") a person-to-person chat: never
  // relayed to a peer. "Add a remark" opens the contact page; the rest runs on the device.
  if (const auto local_tool = LocalActionTool(user_payload)) {
    if (*local_tool == "edit_remark") {
      if (on_local_action_) {
        on_local_action_(text, user_payload);
      }
      return {};
    }
    if (!IsAgentLocalActionPayload(user_payload)) {
      log().warning << "local action payload with an unknown tool ignored: " << *local_tool;
      return {};
    }
    if (!agent_.submit_local_action) {
      return Error("Agent not bound");
    }
    if (AgentBusy()) {
      return Error(kAgentBusyError);
    }
    agent_.submit_local_action(thread_id, text, *user_payload, HandlesAtAi((*thread)->kind));
    return {};
  }

  if (user_payload && !user_payload->empty() && on_local_action_ &&
      !IsAgentToolPayload((*thread)->kind, user_payload)) {
    on_local_action_(text, user_payload);
    return {};
  }

  const AtAiParseResult at_ai = ParseAtAiPrefix(text);
  // A group used to send "@ai++ …" as plain text while the chat already showed "thinking" (B111).
  if (HandlesAtAi((*thread)->kind) && at_ai.is_ai_invoke) {
    if (AgentBusy()) {
      return Error(kAgentBusyError); // nothing sent, no confirm asked: the UI queues it
    }
    if (at_ai.mode == AtAiMode::SharedReply || at_ai.mode == AtAiMode::SharedFull) {
      if (agent_.is_app_action_request && agent_.submit_scoped_assist && agent_.is_app_action_request(at_ai.prompt)) {
        // "@ai++ 搜索用户 ken": an app action is not shared. Nothing goes to the peer (no prompt, no confirm); the
        // agent says so on this device.
        agent_.submit_scoped_assist(thread_id, at_ai.prompt, {}, at_ai.mode);
        return {};
      }
      if (NeedsSharedAiConfirmFor(thread_id, text)) { // the one rule the UI also asks before it sends
        shared_ai_confirm_(thread_id, at_ai.mode, at_ai.prompt,
                           [this, thread_id, mode = at_ai.mode, prompt = at_ai.prompt](
                               const bool confirmed, const bool dont_ask_again) {
                             if (!confirmed) {
                               return;
                             }
                             if (dont_ask_again) {
                               MarkSharedAiConfirmed(thread_id);
                             }
                             if (auto routed = RouteSharedAi(thread_id, prompt, mode); !routed) {
                               log().warning << "Confirmed shared AI send failed: " << routed.error().message;
                             }
                           });
        return {};
      }
      return RouteSharedAi(thread_id, at_ai.prompt, at_ai.mode);
    }
    if (!agent_.submit_scoped_assist) {
      return Error("Agent not bound");
    }
    agent_.submit_scoped_assist(thread_id, at_ai.prompt, {}, AtAiMode::Local);
    return {};
  }

  if ((*thread)->kind == ThreadKind::Direct) {
    SendRelayOptions opts;
    auto sent = mesh_messaging_.SendUserMessage(thread_id, text, opts);
    if (!sent) {
      return sent.error();
    }
    return {};
  }

  if ((*thread)->kind == ThreadKind::Group) {
    SendRelayOptions group_opts;
    auto sent = mesh_messaging_.SendGroupMessage(thread_id, text, group_opts);
    if (!sent) {
      return sent.error();
    }
    return {};
  }

  if (!agent_.submit_to_thread) {
    return Error("Agent not bound");
  }
  if (AgentBusy()) {
    return Error(kAgentBusyError);
  }
  agent_.submit_to_thread(thread_id, TextForAgent((*thread)->kind, text), std::move(user_payload));
  return {};
}

Roe<void> MessageRouter::RouteConfirmedSharedAi(const std::string& thread_id, const std::string& text) {
  const AtAiParseResult at_ai = ParseAtAiPrefix(text);
  if (!at_ai.is_ai_invoke || (at_ai.mode != AtAiMode::SharedReply && at_ai.mode != AtAiMode::SharedFull)) {
    return Route(thread_id, text);
  }
  return RouteSharedAi(thread_id, at_ai.prompt, at_ai.mode);
}

bool MessageRouter::NeedsSharedAiConfirmFor(const std::string& thread_id, const std::string& text) const {
  if (!shared_ai_confirm_) {
    return false;
  }
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread || !HandlesAtAi((*thread)->kind)) {
    return false;
  }
  const AtAiParseResult at_ai = ParseAtAiPrefix(text);
  if (!at_ai.is_ai_invoke || (at_ai.mode != AtAiMode::SharedReply && at_ai.mode != AtAiMode::SharedFull)) {
    return false;
  }
  // An app action ("@ai++ 搜索用户 ken") shares nothing and is never confirmed.
  if (agent_.is_app_action_request && agent_.is_app_action_request(at_ai.prompt)) {
    return false;
  }
  return NeedsSharedAiConfirm(thread_id);
}

std::string MessageRouter::TextForAgent(const ThreadKind kind, const std::string& text) {
  // In an AI thread "@ai …" is just a habit from person-to-person chats: send the question itself.
  if (kind != ThreadKind::Ai) {
    return text;
  }
  const AtAiParseResult at_ai = ParseAtAiPrefix(text);
  return at_ai.is_ai_invoke ? at_ai.prompt : text;
}

bool MessageRouter::HandlesAtAi(const ThreadKind kind) {
  return kind == ThreadKind::Direct || kind == ThreadKind::Group;
}

bool MessageRouter::ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                                     const std::optional<std::string>& user_payload) const {
  if (text.empty()) {
    return false;
  }
  auto thread = store_.GetThread(thread_id);
  if (IsAgentLocalActionPayload(user_payload)) {
    return true;
  }
  if (user_payload && !user_payload->empty()) {
    return thread && *thread && IsAgentToolPayload((*thread)->kind, user_payload);
  }
  if (!thread || !*thread) {
    return true;
  }

  if (HandlesAtAi((*thread)->kind)) {
    return ParseAtAiPrefix(text).is_ai_invoke;
  }
  return true;
}

} // namespace pbr
