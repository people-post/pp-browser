#include "messaging/MessageRouter.h"

#include "messaging/AtAiParser.h"
#include "messaging/IThreadStore.h"
#include "messaging/IdUtil.h"
#include "messaging/MessagingHub.h"
#include "messaging/ThreadTypes.h"

namespace pbr {

MessageRouter::MessageRouter(InboxController& inbox, P2pMessagingService& p2p, AgentSession& agent)
    : inbox_(inbox), p2p_(p2p), agent_(agent) {
  redirectLogger("MessageRouter");
}

void MessageRouter::SetOnLocalAction(
    std::function<void(const std::string& message, const std::optional<std::string>& payload)> cb) {
  on_local_action_ = std::move(cb);
}

bool MessageRouter::ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                                     const std::optional<std::string>& user_payload) const {
  if (text.empty()) {
    return false;
  }
  if (user_payload && !user_payload->empty()) {
    return false;
  }

  auto thread = MessagingHub::Instance().Store().GetThread(thread_id);
  if (!thread || !*thread) {
    return true;
  }

  if ((*thread)->kind == ThreadKind::Direct) {
    return ParseAtAiPrefix(text).is_ai_invoke;
  }
  return true;
}

Roe<void> MessageRouter::Route(const std::string& thread_id, const std::string& text,
                               std::optional<std::string> user_payload) {
  if (text.empty()) {
    return Error("Empty message");
  }

  auto thread = MessagingHub::Instance().Store().GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  if (user_payload && !user_payload->empty() && on_local_action_) {
    on_local_action_(text, user_payload);
    return {};
  }

  const AtAiParseResult at_ai = ParseAtAiPrefix(text);
  if ((*thread)->kind == ThreadKind::Direct && at_ai.is_ai_invoke) {
    agent_.SubmitScopedAssist(thread_id, at_ai.prompt, {});
    return {};
  }

  if ((*thread)->kind == ThreadKind::Direct) {
    auto sent = p2p_.SendUserMessage(thread_id, text);
    if (!sent) {
      return sent.error();
    }
    return {};
  }

  agent_.SubmitToThread(thread_id, text, std::move(user_payload));
  return {};
}

} // namespace pbr
