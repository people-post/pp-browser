#include "feature/messaging/MessageRouter.h"

#include "base/messaging/AtAiParser.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/SendRelayOptions.h"
#include "common/Utilities.h"
#include "feature/messaging/MessagingHub.h"
#include "base/messaging/ThreadTypes.h"

namespace pbr {

MessageRouter::MessageRouter(InboxController& inbox, P2pMessagingService& p2p, AgentSession& agent)
    : inbox_(inbox), p2p_(p2p), agent_(agent) {
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

Roe<void> MessageRouter::RouteSharedAi(const std::string& thread_id, const std::string& prompt, const AtAiMode mode) {
  auto thread = MessagingHub::Instance().Store().GetThread(thread_id);
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
    user_opts.seq_owner_contact_id = seq_owner;
    auto sent = p2p_.SendUserMessage(thread_id, prompt, user_opts);
    if (!sent) {
      return sent.error();
    }
  }

  agent_.SubmitScopedAssist(thread_id, prompt, {}, mode);
  return {};
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
    if (at_ai.mode == AtAiMode::SharedReply || at_ai.mode == AtAiMode::SharedFull) {
      if (NeedsSharedAiConfirm(thread_id) && shared_ai_confirm_) {
        shared_ai_confirm_(thread_id, at_ai.mode, at_ai.prompt,
                           [this, thread_id, mode = at_ai.mode, prompt = at_ai.prompt](
                               const bool confirmed, const bool dont_ask_again) {
                             if (!confirmed) {
                               return;
                             }
                             if (dont_ask_again) {
                               MarkSharedAiConfirmed(thread_id);
                             }
                             RouteSharedAi(thread_id, prompt, mode);
                           });
        return {};
      }
      return RouteSharedAi(thread_id, at_ai.prompt, at_ai.mode);
    }
    agent_.SubmitScopedAssist(thread_id, at_ai.prompt, {}, AtAiMode::Local);
    return {};
  }

  if ((*thread)->kind == ThreadKind::Direct) {
    SendRelayOptions opts;
    auto sent = p2p_.SendUserMessage(thread_id, text, opts);
    if (!sent) {
      return sent.error();
    }
    return {};
  }

  if ((*thread)->kind == ThreadKind::Group) {
    SendRelayOptions group_opts;
    auto sent = p2p_.SendGroupMessage(thread_id, text, group_opts);
    if (!sent) {
      return sent.error();
    }
    return {};
  }

  agent_.SubmitToThread(thread_id, text, std::move(user_payload));
  return {};
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
  if ((*thread)->kind == ThreadKind::Group) {
    return ParseAtAiPrefix(text).is_ai_invoke;
  }
  return true;
}

} // namespace pbr
