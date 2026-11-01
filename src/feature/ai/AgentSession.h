#pragma once

#include "lib/messaging/AtAiParser.h"
#include "lib/ai/BriefAiClient.h"
#include "lib/ai/LocalActionPlanner.h"
#include "lib/ai/ToolRegistry.h"
#include "lib/ai/TurnPlan.h"
#include "lib/ai/TurnTrace.h"
#include "common/ShareableLine.h"
#include "lib/ai/conversation/ConversationTypes.h"
#include "lib/ai/mcp/McpClient.h"
#include "foundation/data/Config.h"
#include "foundation/data/ToolPermissions.h"
#include "common/Error.h"
#include "lib/ai/BriefTurnRunner.h"
#include "lib/ai/ParkedApproval.h"
#include "feature/ai/TurnExecutor.h"

#include <chrono>
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
/** The local contacts, flattened for the on-device app-action planner (called on a worker thread). */
using LocalContactsFn = std::function<std::vector<LocalPlanContact>()>;

enum class AgentEventType {
  LoadingChanged,
  ToolActivity,
  AssistantDelta, // text = accumulated streamed text so far
  AssistantReady,
  Error,
  /** A permission card ended: `entry_id` = the stored card message, `approval_id`, `thread_id`, `approval_outcome`. */
  ApprovalClosed,
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
  std::vector<BriefAiSource> sources; // AssistantReady of a streamed answer
  /** AssistantReady: false only for blocks the host built itself; the GUI filters privileged payloads of the rest (PB-007). */
  bool from_model = true;
  bool retryable = false;             // Error
  /** Error of an image turn the GUI words itself: "image_too_large" (HTTP 413) or "image_unsupported" (HTTP 400). */
  std::string error_kind;
  std::string approval_id;                                   // ApprovalClosed
  ApprovalOutcome approval_outcome = ApprovalOutcome::Stale; // ApprovalClosed
};

/** Prefix of a stored user message that carried an image; later history sends it as plain text. */
inline constexpr const char* kAiImageTurnMarker = "[Image] ";

/**
 * One image sent with a question. `message_id` (optional) is the id the user message is stored under;
 * `ai_image_json` (optional) is stored on that message so the GUI can show the image again later.
 */
struct AgentImageTurn {
  BriefAiImage image;
  std::string message_id;
  std::string ai_image_json;
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
  /** Replace the brief_AI stream (tests / fake server); otherwise built from the LLM config in Configure. */
  void SetBriefAiStream(BriefAiStreamFn fn);
  /**
   * How long a turn whose planned tool is missing waits for the promoted MCP server to list it, re-listing every
   * `poll` (default 10 s / 500 ms; tests shorten it).
   */
  void SetMissingToolWait(std::chrono::milliseconds total, std::chrono::milliseconds poll);
  /**
   * Where the on-device app-action planner reads contacts from (names are matched against remark and nickname
   * on the device; nothing of it is sent anywhere). Unset: no contacts.
   */
  void SetLocalContactsSource(LocalContactsFn source);
  /** How long a question the app asked back waits for its answer (default 2 min; tests shorten it). */
  void SetLocalAskTtl(std::chrono::milliseconds ttl);
  /**
   * A button the on-device flow rendered ({"type":"local_action",...}): runs `tool` with `args` on the device —
   * no classification, no model. Only open_conversation, start_conversation and search_people; anything else is
   * ignored with a warning. `user_text` is the button's label (stored as the user's turn in an AI thread).
   * `in_peer_chat`: the button sits in a person-to-person chat; the answer stays on this device there, like "@ai".
   */
  void SubmitLocalAction(const std::string& thread_id, const std::string& user_text, const std::string& tool,
                         Object args, bool in_peer_chat = false);
  /**
   * Brief preset only: does `text` ask to operate the app (the wordlist hits a declared tool)? A shared "@ai+" /
   * "@ai++" with such a request is answered on the device and never relayed. Safe from the UI thread.
   */
  bool IsAppActionRequest(const std::string& text) const;
  void Submit(const std::string& user_text, std::optional<std::string> user_payload = std::nullopt);
  /**
   * `image`: one prepared image sent with this question (brief preset only; any other preset fails the turn).
   * The thread stores the question as kAiImageTurnMarker + text, never the bytes.
   */
  void SubmitToThread(const std::string& thread_id, const std::string& user_text,
                      std::optional<std::string> user_payload = std::nullopt,
                      std::optional<AgentImageTurn> image = std::nullopt);
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
  /** A turn is running (any thread). The UI checks it before routing an AI send so nothing is dropped (B112). */
  bool IsBusy() const;
  /** Block until in-flight ConfigureOnIO finishes (not callable from the IO thread). */
  void WaitForConfigureIdle();
  void StartNewConversation();
  /** The thread was cleared or deleted: forget the brief_AI role that answered its last turn. Any thread. */
  void ForgetThread(const std::string& thread_id);

  bool IsConfigured() const;
  const Conversation& conversation() const;

  TranscriptEntry& AppendUserMessage(const std::string& user_text,
                                     std::optional<std::string> user_payload = std::nullopt);
  bool CompleteAssistantMessage(const std::string& entry_id, const std::string& assistant_raw);
  bool SetAssistantDisplay(const std::string& entry_id, const std::string& assistant_rml,
                           std::vector<TranscriptChatAction> chat_actions);

private:
  struct Impl;

  void BeginStreamTurn();
  static void ConfigureOnIO(const std::shared_ptr<Impl>& state);
  /** Submit worker: true to start the turn now; false when ConfigureOnIO will start it, or (configure failed) it ended. */
  static bool StartNowOrWhenConfigured(const std::shared_ptr<Impl>& state);
  static void StartTurn(const std::shared_ptr<Impl>& state);
  /** Model pipeline (planner + synthesis). Never for the brief preset: its gateway has no completions route (B99). */
  static void RunTurnPipeline(const std::shared_ptr<Impl>& state);
  /** A turn that does not stream: the brief preset runs its payload plan only, other presets the model pipeline. */
  static void RunNonStreamTurn(const std::shared_ptr<Impl>& state);
  /** Brief preset, payload turn: the payload's plan on the device, no planner and no synthesis. */
  static void RunBriefPayloadTurn(const std::shared_ptr<Impl>& state);
  /** Runs a resolved plan through permissions and execution, then ContinueAfterExecution. */
  static void ExecutePlan(const std::shared_ptr<Impl>& state, TurnPlan plan);
  static bool UseBriefStream(const std::shared_ptr<Impl>& state);
  /** The brief preset with its stream wired and no tool payload on this turn. */
  static bool BriefStreamAvailable(const std::shared_ptr<Impl>& state);
  /** Tools brief_AI may hand a turn back for: the messaging and settings providers. */
  static std::vector<std::string> AppActionCapabilities(const std::shared_ptr<Impl>& state);
  /** Judges the pending text on the device; on a hit runs the local pipeline and returns true. */
  static bool TryLocalAppAction(const std::shared_ptr<Impl>& state);
  /** The answer to a question the app asked back (same thread, not expired): true when handled here. */
  static bool TryPendingLocalAnswer(const std::shared_ptr<Impl>& state);
  /**
   * brief_AI handed the turn back for `capability`: answered on the device, never streamed again. `reason` is the
   * server's ("app_action", "user_confirmed"); "user_confirmed" on open_support (the AI offered PP Support, the user
   * said yes) opens it without a card.
   */
  static void HandleHandoff(const std::shared_ptr<Impl>& state, const std::string& capability,
                            const std::string& reason);
  /** Acts on a local plan: execute, ask back, list candidates or say it cannot. `outcome` is the metric for a plan. */
  static void HandleLocalPlan(const std::shared_ptr<Impl>& state, LocalPlan plan, const std::string& tool,
                              ShareWord outcome, bool asked_twice = false);
  static void RunDirectAction(const std::shared_ptr<Impl>& state, const std::string& tool, const Object& args);
  static LocalPlanContext BuildLocalPlanContext(const std::shared_ptr<Impl>& state, const std::string& tool);
  /** Ends the turn with a host-built answer; `keep_out_of_history`: an app-action turn (never sent to a model). */
  static void FinishLocalAnswer(const std::shared_ptr<Impl>& state, const std::string& text, RenderMode mode,
                                bool keep_out_of_history = true);
  /** Template rendering of the executed plan's result; `failed`: the tool failed (`blocked`: by the user's permissions). */
  static void FinishLocalResult(const std::shared_ptr<Impl>& state, bool failed = false, bool blocked = false);
  static void MarkUserEntryLocal(const std::shared_ptr<Impl>& state);
  static void StreamBriefTurn(const std::shared_ptr<Impl>& state, std::vector<BriefAiHistoryTurn> history,
                              std::string summary);
  static void EmitStreamedAnswer(const std::shared_ptr<Impl>& state, const std::string& text,
                                 const std::string& finish_reason, const std::vector<BriefAiSource>& sources);
  /**
   * `cancelled` is set when the turn was stopped while resolving; the error then carries nothing to show.
   * `allow_model` false (brief preset): a turn without a payload plan sets `unplanned` instead of asking the
   * model planner.
   */
  static Roe<TurnPlan> ResolveTurnPlan(const std::shared_ptr<Impl>& state, bool& cancelled, bool allow_model,
                                       bool& unplanned);
  enum class FeedRelist { Listed, Unavailable, Cancelled };
  /**
   * A feed turn whose planned tool is not registered: re-lists the promoted MCP server (when there is one) until
   * a listing answers, the wait runs out or the turn is cancelled. Listed: every planned tool is now registered.
   */
  static FeedRelist RelistMissingFeedTools(const std::shared_ptr<Impl>& state, const TurnPlan& plan);
  static void ContinueAfterExecution(const std::shared_ptr<Impl>& state);
  static void RunSynthesisStep(const std::shared_ptr<Impl>& state);
  static void DispatchRefinementToolCalls(const std::shared_ptr<Impl>& state, const std::vector<ToolCall>& tool_calls,
                                          const std::string& assistant_content);
  static void HandleSynthesisResponse(const std::shared_ptr<Impl>& state, const ChatCompletionResponse& response);
  static void ValidateAndFinishAssistant(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                         const std::string& finish_reason, bool allow_repair, bool from_model = true);
  static void RunOutputRepair(const std::shared_ptr<Impl>& state, const std::string& raw_output,
                              const std::string& parse_error);
  static void FinishAssistantOutput(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                    const std::string& finish_reason, bool from_model = true);
  static void PersistAssistantToThread(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                       std::string* out_message_id = nullptr);
  static void InjectSynthesisPolicy(const std::shared_ptr<Impl>& state);
  static void PopulateTurnTraceFromPlan(const std::shared_ptr<Impl>& state);
  static std::vector<std::string> AllowedToolNames(const std::shared_ptr<Impl>& state);
  /** `only_thread`: cancel only when the parked request belongs to that thread (a turn elsewhere leaves it alone). */
  static void CancelParkedApproval(const std::shared_ptr<Impl>& state, ParkedApprovalState reason,
                                   const std::optional<std::string>& only_thread = std::nullopt);
  static void PushApprovalClosed(const std::shared_ptr<Impl>& state, const ParkedApproval& park,
                                 ApprovalOutcome outcome);
  static void RecordParkedCardEntry(const std::shared_ptr<Impl>& state, const std::string& entry_id);
  static void ParkForPermission(const std::shared_ptr<Impl>& state, TurnExecutionResult execution);
  static void ResumeToolPermissionOnWorker(const std::shared_ptr<Impl>& state, std::string approval_id,
                                           std::string decision, std::string decision_label);
  static ApprovalResumeError ValidateParkedResume(const std::optional<ParkedApproval>& park,
                                                  const std::string& approval_id, bool busy);

  static void PushEvent(const std::shared_ptr<Impl>& state, AgentEvent event);
  /** `thread_id`: the turn's thread (an ended turn passes it explicitly: pending_thread_id is cleared by then). */
  static void PushLoading(const std::shared_ptr<Impl>& state, bool loading, const std::string& thread_id = {});
  static ToolMeta MetaForTool(const std::shared_ptr<Impl>& state, const std::string& name);
  static void PushToolActivity(const std::shared_ptr<Impl>& state, const std::string& tool_name,
                               const std::string& status);
  static void PushAssistantReady(const std::shared_ptr<Impl>& state, const std::string& entry_id,
                                 const std::string& text, const std::string& finish_reason,
                                 std::vector<BriefAiSource> sources = {}, bool from_model = true);
  static void PushError(const std::shared_ptr<Impl>& state, const std::string& message, bool retryable = false,
                        const std::string& error_kind = {});
  static void PushError(const std::shared_ptr<Impl>& state, const Error& err);
  static void FinishTurn(const std::shared_ptr<Impl>& state);
  static void RefreshCompactionService(const std::shared_ptr<Impl>& state);

  std::shared_ptr<Impl> impl_;
};

} // namespace pbr
