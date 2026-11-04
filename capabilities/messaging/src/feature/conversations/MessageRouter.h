#pragma once

#include "lib/messaging/AtAiParser.h"
#include "common/thread/IThreadStore.h"
#include "common/Module.h"
#include "feature/conversations/AgentInboundPorts.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "feature/conversations/SharedAiConsent.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class MessageRouter : public Module {
public:
  MessageRouter(InboxController& inbox, MeshDeliveryOrchestrator& mesh_messaging, AgentInboundPorts agent,
                IThreadStore& store);
  /** The agent's entry points (empty: none — a send that needs the agent fails "Agent not bound"). */
  void SetAgent(AgentInboundPorts agent) { agent_ = std::move(agent); }

  /** Error message of a send that needs the agent while a turn runs: nothing was stored or sent; queue and retry. */
  static constexpr const char* kAgentBusyError = "agent_busy";

  Roe<void> Route(const std::string& thread_id, const std::string& text,
                  std::optional<std::string> user_payload = std::nullopt);
  /** "@ai+ …" / "@ai++ …" the user already confirmed (before it was queued): shared without asking again. */
  Roe<void> RouteConfirmedSharedAi(const std::string& thread_id, const std::string& text);
  /** Routing `text` in `thread_id` would ask the user to confirm sharing with the AI first. */
  bool NeedsSharedAiConfirmFor(const std::string& thread_id, const std::string& text) const;

  /**
   * True for a payload the agent runs as a tool call ({"tool":"…"} without a "type") instead of a local
   * action. AI threads only: in a peer thread nothing would run it and its text would go to the peer.
   */
  static bool IsAgentToolPayload(ThreadKind kind, const std::optional<std::string>& user_payload);
  /**
   * The tool of an on-device app-action button payload ({"type":"local_action","tool":…}), else nullopt.
   * "edit_remark" is the UI's (the contact page); open_conversation / start_conversation / search_people go to the
   * agent; any other tool is dropped with a warning.
   */
  static std::optional<std::string> LocalActionTool(const std::optional<std::string>& user_payload);
  /** True when a local-action payload is run by the agent (the three tools above), in any thread kind. */
  static bool IsAgentLocalActionPayload(const std::optional<std::string>& user_payload);
  static Roe<void> ValidateUserPayload(const std::optional<std::string>& user_payload);
  /** What the agent receives for a message typed in a thread: in an AI thread an "@ai…" prefix is dropped. */
  static std::string TextForAgent(ThreadKind kind, const std::string& text);
  /** "@ai" / "@ai+" / "@ai++" are understood in the person-to-person threads: direct and group. */
  static bool HandlesAtAi(ThreadKind kind);

  bool ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                        const std::optional<std::string>& user_payload = std::nullopt) const;

  void SetOnLocalAction(std::function<void(const std::string& message, const std::optional<std::string>& payload)> cb);

  using SharedAiConfirmCallback =
      std::function<void(const std::string& thread_id, AtAiMode mode, const std::string& prompt,
                         std::function<void(bool confirmed, bool dont_ask_again)> done)>;
  void SetSharedAiConfirmCallback(SharedAiConfirmCallback callback);
  void MarkSharedAiConfirmed(const std::string& thread_id);
  /** The user is to be asked again for this chat (the session's tick; the stored setting is cleared by the caller). */
  void ClearSharedAiConfirmed(const std::string& thread_id);
  /** Whether a chat's "don't ask again" is stored (per chat, kept across restarts); read on every check. */
  void SetSharedAiConfirmedPort(SharedAiConsent::StoredPort port);

private:
  bool AgentBusy(const std::string& thread_id) const { return agent_.is_busy && agent_.is_busy(thread_id); }
  Roe<void> RouteSharedAi(const std::string& thread_id, const std::string& prompt, AtAiMode mode);
  bool NeedsSharedAiConfirm(const std::string& thread_id) const;

  MeshDeliveryOrchestrator& mesh_messaging_;
  AgentInboundPorts agent_;
  IThreadStore& store_;
  std::function<void(const std::string&, const std::optional<std::string>&)> on_local_action_;
  SharedAiConfirmCallback shared_ai_confirm_;
  SharedAiConsent shared_ai_consent_;
};

} // namespace pbr
