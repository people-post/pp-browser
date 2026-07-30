#pragma once

#include "base/messaging/AtAiParser.h"
#include "base/messaging/IThreadStore.h"
#include "common/Module.h"
#include "feature/messaging/InboxController.h"
#include "feature/messaging/P2pMessagingService.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>

namespace pbr {

class AgentSession;

class MessageRouter : public Module {
public:
  MessageRouter(InboxController& inbox, P2pMessagingService& p2p, AgentSession& agent, IThreadStore& store);

  Roe<void> Route(const std::string& thread_id, const std::string& text,
                  std::optional<std::string> user_payload = std::nullopt);

  bool ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                        const std::optional<std::string>& user_payload = std::nullopt) const;

  void SetOnLocalAction(std::function<void(const std::string& message, const std::optional<std::string>& payload)> cb);

  using SharedAiConfirmCallback =
      std::function<void(const std::string& thread_id, AtAiMode mode, const std::string& prompt,
                         std::function<void(bool confirmed, bool dont_ask_again)> done)>;
  void SetSharedAiConfirmCallback(SharedAiConfirmCallback callback);
  void MarkSharedAiConfirmed(const std::string& thread_id);

private:
  Roe<void> RouteSharedAi(const std::string& thread_id, const std::string& prompt, AtAiMode mode);
  bool NeedsSharedAiConfirm(const std::string& thread_id) const;

  InboxController& inbox_;
  P2pMessagingService& p2p_;
  AgentSession& agent_;
  IThreadStore& store_;
  std::function<void(const std::string&, const std::optional<std::string>&)> on_local_action_;
  SharedAiConfirmCallback shared_ai_confirm_;
  std::unordered_set<std::string> shared_ai_confirmed_threads_;
};

} // namespace pbr
