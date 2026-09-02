#pragma once

#include "domain/messaging/AtAiParser.h"
#include "domain/ai/ToolRegistry.h"
#include "domain/ai/TurnPlan.h"
#include "domain/ai/TurnTrace.h"
#include "domain/ai/conversation/ConversationTypes.h"
#include "domain/ai/mcp/McpClient.h"
#include "foundation/data/Config.h"
#include "foundation/data/ToolPermissions.h"
#include "common/Error.h"
#include "feature/ai/ParkedApproval.h"
#include "feature/ai/TurnExecutor.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class Conversation;
class IThreadStore;

using ToolRegistrationHook = std::function<void(ToolRegistry&)>;
using ToolPermissionsSaveFn = std::function<Roe<void>(const ToolPermissionsPrefs&)>;

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
  AtAiMode shared_ai_mode = AtAiMode::None;
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
  void SetToolRegistrationHook(ToolRegistrationHook hook);
  void SetToolPermissions(ToolPermissionsPrefs permissions);
  void SetToolPermissionsSaver(ToolPermissionsSaveFn saver);
  McpClient* PromotedMcp();
  void SetThreadStore(IThreadStore* store);
  void Submit(const std::string& user_text, std::optional<std::string> user_payload = std::nullopt);
  void SubmitToThread(const std::string& thread_id, const std::string& user_text,
                      std::optional<std::string> user_payload = std::nullopt);
  void SubmitScopedAssist(const std::string& thread_id, const std::string& prompt,
                          std::optional<std::string> user_payload = std::nullopt,
                          AtAiMode mode = AtAiMode::Local);
  /**
   * Resume a parked tool-permission confirm from an in-chat choice payload.
   * decision: allow_once | allow_always | deny
   */
  Roe<void> ResumeToolPermission(const std::string& approval_id, const std::string& decision,
                                 const std::string& decision_label = {});
  void PollEvents(std::vector<AgentEvent>& out);
  void Cancel();
  /** Block until in-flight ConfigureOnIO finishes (not callable from the IO thread). */
  void WaitForConfigureIdle();
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
  static void CancelParkedApproval(const std::shared_ptr<Impl>& state, ParkedApprovalState reason);
  static void ParkForPermission(const std::shared_ptr<Impl>& state, TurnExecutionResult execution);
  static void ResumeToolPermissionOnWorker(const std::shared_ptr<Impl>& state, std::string approval_id,
                                           std::string decision, std::string decision_label);
  static ApprovalResumeError ValidateParkedResume(const std::optional<ParkedApproval>& park,
                                                  const std::string& approval_id, bool busy);

  static void PushEvent(const std::shared_ptr<Impl>& state, AgentEvent event);
  static void PushLoading(const std::shared_ptr<Impl>& state, bool loading);
  static void PushToolActivity(const std::shared_ptr<Impl>& state, const std::string& tool_name,
                               const std::string& status);
  static void PushAssistantReady(const std::shared_ptr<Impl>& state, const std::string& entry_id,
                                 const std::string& text, const std::string& finish_reason);
  static void PushError(const std::shared_ptr<Impl>& state, const std::string& message);
  static void PushError(const std::shared_ptr<Impl>& state, const Error& err);
  static void FinishTurn(const std::shared_ptr<Impl>& state);
  static void RefreshCompactionService(const std::shared_ptr<Impl>& state);

  std::shared_ptr<Impl> impl_;
};

} // namespace pbr
