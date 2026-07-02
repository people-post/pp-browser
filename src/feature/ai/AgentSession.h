#pragma once

#include "base/ai/LlmClient.h"
#include "base/ai/TurnPlan.h"
#include "base/ai/TurnTrace.h"
#include "base/ai/conversation/ConversationTypes.h"
#include "base/ai/mcp/McpClient.h"
#include "base/data/Config.h"
#include "common/Error.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

class Conversation;
class IThreadStore;

enum class AgentEventType {
  LoadingChanged,
  ToolActivity,
  AssistantReady,
  Error,
};

struct AgentEvent {
  AgentEventType type = AgentEventType::LoadingChanged;
  bool loading = false;
  std::string tool_name;
  std::string status;
  std::string text;
  std::string message;
  std::string entry_id;
  std::string thread_id;
  std::string finish_reason;
  bool scoped_assist = false;
  ResponseGoal response_goal = ResponseGoal::General;
  RenderMode render_mode = RenderMode::Blocks;
};

class AgentSession {
public:
  AgentSession();
  ~AgentSession();

  AgentSession(const AgentSession&) = delete;
  AgentSession& operator=(const AgentSession&) = delete;

  void Configure(const AppConfig& config);
  McpClient* PromotedMcp();
  void SetThreadStore(IThreadStore* store);
  void Submit(const std::string& user_text, std::optional<std::string> user_payload = std::nullopt);
  void SubmitToThread(const std::string& thread_id, const std::string& user_text,
                      std::optional<std::string> user_payload = std::nullopt);
  void SubmitScopedAssist(const std::string& thread_id, const std::string& prompt,
                          std::optional<std::string> user_payload = std::nullopt);
  void PollEvents(std::vector<AgentEvent>& out);
  void Cancel();
  void StartNewConversation();

  bool IsConfigured() const;
  const Conversation& conversation() const;

  TranscriptEntry& AppendUserMessage(const std::string& user_text,
                                     std::optional<std::string> user_payload = std::nullopt);
  bool CompleteAssistantMessage(const std::string& entry_id, const std::string& assistant_raw);
  bool SetAssistantDisplay(const std::string& entry_id, const std::string& assistant_rml,
                           std::vector<TranscriptChatAction> chat_actions);

private:
  struct Impl;

  static void ConfigureOnIO(const std::shared_ptr<Impl>& state);
  static void StartTurn(const std::shared_ptr<Impl>& state);
  static void RunTurnPipeline(const std::shared_ptr<Impl>& state);
  static Roe<TurnPlan> ResolveTurnPlan(const std::shared_ptr<Impl>& state);
  static void ContinueAfterExecution(const std::shared_ptr<Impl>& state);
  static void RunSynthesisStep(const std::shared_ptr<Impl>& state);
  static void DispatchRefinementToolCalls(const std::shared_ptr<Impl>& state, const std::vector<ToolCall>& tool_calls,
                                          const std::string& assistant_content);
  static void HandleSynthesisResponse(const std::shared_ptr<Impl>& state, const ChatCompletionResponse& response);
  static void ValidateAndFinishAssistant(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                         const std::string& finish_reason, bool allow_repair);
  static void RunOutputRepair(const std::shared_ptr<Impl>& state, const std::string& raw_output,
                              const std::string& parse_error);
  static void FinishAssistantOutput(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                    const std::string& finish_reason);
  static void PersistAssistantToThread(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                       std::string* out_message_id = nullptr);
  static void InjectSynthesisPolicy(const std::shared_ptr<Impl>& state);
  static void PopulateTurnTraceFromPlan(const std::shared_ptr<Impl>& state);
  static std::vector<std::string> AllowedToolNames(const std::shared_ptr<Impl>& state);

  static void PushEvent(const std::shared_ptr<Impl>& state, AgentEvent event);
  static void PushLoading(const std::shared_ptr<Impl>& state, bool loading);
  static void PushToolActivity(const std::shared_ptr<Impl>& state, const std::string& tool_name,
                               const std::string& status);
  static void PushAssistantReady(const std::shared_ptr<Impl>& state, const std::string& entry_id,
                                 const std::string& text, const std::string& finish_reason);
  static void PushError(const std::shared_ptr<Impl>& state, const std::string& message);
  static void FinishTurn(const std::shared_ptr<Impl>& state);
  static void RefreshCompactionService(const std::shared_ptr<Impl>& state);

  std::shared_ptr<Impl> impl_;
};

} // namespace pbr
