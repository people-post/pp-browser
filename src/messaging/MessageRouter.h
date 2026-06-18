#pragma once

#include "agent/AgentSession.h"
#include "common/Module.h"
#include "messaging/InboxController.h"
#include "messaging/P2pMessagingService.h"

#include <functional>
#include <optional>
#include <string>

namespace pbr {

class MessageRouter : public Module {
public:
  MessageRouter(InboxController& inbox, P2pMessagingService& p2p, AgentSession& agent);

  Roe<void> Route(const std::string& thread_id, const std::string& text,
                  std::optional<std::string> user_payload = std::nullopt);

  bool ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                        const std::optional<std::string>& user_payload = std::nullopt) const;

  void SetOnLocalAction(std::function<void(const std::string& message, const std::optional<std::string>& payload)> cb);

private:
  InboxController& inbox_;
  P2pMessagingService& p2p_;
  AgentSession& agent_;
  std::function<void(const std::string&, const std::optional<std::string>&)> on_local_action_;
};

} // namespace pbr
