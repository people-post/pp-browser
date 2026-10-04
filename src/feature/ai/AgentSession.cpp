#include "feature/ai/AgentSession.h"
#include "foundation/platform/Platform.h"

#include "domain/ai/AppActionClassifier.h"
#include "domain/ai/LocalizedLabels.h"
#include "domain/ai/PayloadTurnPlanBuilder.h"
#include "domain/ai/PromptBuilder.h"
#include "domain/ai/StructuredTextParser.h"
#include "domain/ai/ToolRegistry.h"
#include "domain/ai/ToolResultFormatter.h"
#include "feature/ai/ParkedApproval.h"
#include "feature/ai/ToolPermissionPolicy.h"
#include "feature/ai/ToolPermissionPrompt.h"
#include "feature/ai/ToolRegistryBuild.h"
#include "feature/ai/TurnExecutor.h"
#include "feature/ai/TurnPlanner.h"
#include "domain/ai/conversation/Conversation.h"
#include "domain/ai/conversation/ThreadCompactionService.h"
#include "domain/ai/conversation/ThreadContextPolicy.h"
#include "domain/ai/conversation/TurnCoordinator.h"
#include "foundation/error/AppError.h"
#include "domain/ai/mcp/McpClient.h"
#include "domain/net/HttpClient.h"

#include <map>
#include "domain/ai/mcp/McpRuntime.h"
#include "foundation/data/Config.h"
#include "foundation/data/LlmPreset.h"
#include "common/Logger.h"
#include "common/Module.h"
#include "common/Utilities.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/runtime/AppVersion.h"
#include "common/Metrics.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "common/ValueJson.h"

#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr int kMaxIterations = 8;

Value ToolCallsToJson(const std::vector<ToolCall>& tool_calls) {
  std::vector<Value> out;
  out.reserve(tool_calls.size());
  for (const ToolCall& call : tool_calls) {
    Object function;
    function.set("name", call.name);
    function.set("arguments", DumpJson(call.arguments));
    Object entry;
    entry.set("id", call.id);
    entry.set("type", "function");
    entry.set("function", function);
    out.push_back(ObjectValue(std::move(entry)));
  }
  return ArrayValue(std::move(out));
}

void AppendSynthesisReminder(std::vector<ChatMessage>& messages, const TurnPlan& plan) {
  if (messages.empty() || messages.back().role != "tool") {
    return;
  }
  messages.back().content += "\n\n" + PromptBuilder::BuildSynthesisRefinementReminder(plan);
}

} // namespace

struct AgentSession::Impl : public Module {
  Impl() { redirectLogger("AgentSession"); }

  logging::Logger& Log() const { return log(); }

  std::mutex event_mutex;
  std::vector<AgentEvent> pending_events;

  std::atomic<bool> cancelled{false};
  /** Streamed turns: a fresh cancel flag per submit, so a stopped stream can never act on a later turn. */
  std::mutex stream_cancel_mutex;
  std::shared_ptr<std::atomic<bool>> stream_cancel = std::make_shared<std::atomic<bool>>(false);
  std::atomic<uint64_t> turn_generation{0};
  /** The current turn was judged an "app action" (here or by brief_AI); FinishTurn reports whether tools ran. */
  bool handoff_turn = false;
  /** ...and it was judged on this device rather than by a handoff event. */
  bool handoff_local = false;
  std::atomic<bool> busy{false};
  std::mutex configure_mutex;
  std::condition_variable configure_cv;
  int configure_inflight = 0;
  /** Serialize ConfigureOnIO body — concurrent Configure() raced mcp.Start/Stop (UAF → SEGV in logger). */
  std::mutex configure_work_mutex;

  AppConfig config;
  std::unique_ptr<LlmClient> llm;
  BriefAiStreamFn brief_stream;          // built in ConfigureOnIO
  BriefAiStreamFn brief_stream_override; // SetBriefAiStream; wins over brief_stream
  McpRuntime mcp;
  ToolRegistry tools;
  Conversation conversation;
  TurnCoordinator coordinator;
  std::vector<ChatMessage> turn_scratch;
  TurnPlan turn_plan;
  TurnTrace turn_trace;
  std::optional<std::string> people_list_blocks;
  std::string pending_user_text;
  std::optional<std::string> pending_user_payload;
  std::optional<AgentImageTurn> pending_image; // at most one per turn; SubmitToThread only
  std::string pending_entry_id;
  int iterations = 0;
  bool configured = false;
  bool submit_when_ready = false;
  std::chrono::steady_clock::time_point planner_started{};
  std::chrono::steady_clock::time_point synthesis_started{};

  IThreadStore* thread_store = nullptr;
  std::unique_ptr<ThreadCompactionService> compaction;
  std::string pending_thread_id;
  AgentTurnMode turn_mode = AgentTurnMode::Conversation;
  AtAiMode assist_mode = AtAiMode::Local;
  ToolRegistrationHook tool_registration_hook;
  ToolPermissionsPrefs tool_permissions;
  ToolPermissionsSaveFn tool_permissions_saver;
  std::optional<ParkedApproval> parked_approval;
  std::mutex park_mutex;
};

void AgentSession::PushEvent(const std::shared_ptr<Impl>& state, AgentEvent event) {
  AppRuntime::PostUI([state, event = std::move(event)]() mutable {
    std::lock_guard lock(state->event_mutex);
    state->pending_events.push_back(std::move(event));
  });
}

void AgentSession::PushLoading(const std::shared_ptr<Impl>& state, const bool loading) {
  PushEvent(state, AgentEvent{.type = AgentEventType::LoadingChanged, .loading = loading});
}

void AgentSession::PushToolActivity(const std::shared_ptr<Impl>& state, const std::string& tool_name,
                                    const std::string& status) {
  PushEvent(state, AgentEvent{.type = AgentEventType::ToolActivity, .tool_name = tool_name, .status = status});
}

void AgentSession::PushAssistantReady(const std::shared_ptr<Impl>& state, const std::string& entry_id,
                                      const std::string& text, const std::string& finish_reason,
                                      std::vector<BriefAiSource> sources) {
  PushEvent(state, AgentEvent{.type = AgentEventType::AssistantReady,
                              .text = text,
                              .entry_id = entry_id,
                              .thread_id = state->pending_thread_id,
                              .finish_reason = finish_reason,
                              .scoped_assist = state->turn_mode == AgentTurnMode::ScopedAssist,
                              .shared_ai_mode = state->assist_mode,
                              .response_goal = state->turn_plan.response_goal,
                              .render_mode = state->turn_plan.render_mode,
                              .sources = std::move(sources)});
}

void AgentSession::PushError(const std::shared_ptr<Impl>& state, const std::string& message, const bool retryable,
                             const std::string& error_kind) {
  PushEvent(state, AgentEvent{
                       .type = AgentEventType::Error, .message = message, .retryable = retryable, .error_kind = error_kind});
}

void AgentSession::PushError(const std::shared_ptr<Impl>& state, const Error& err) {
  state->Log().error << "Agent error " << AppError::Log(err);
  PushError(state, AppError::Display(err));
}

void AgentSession::FinishTurn(const std::shared_ptr<Impl>& state) {
  state->pending_image.reset(); // up to ~1.5 MB; not needed once the turn is over
  state->turn_trace.Log();
  if (state->handoff_turn) {
    bool parked = false;
    {
      std::lock_guard lock(state->park_mutex);
      parked = state->parked_approval && state->parked_approval->state == ParkedApprovalState::Pending;
    }
    // A handoff that ran no tool locally is the signal for a mis-routed question (plan step 5). A turn
    // parked on a permission prompt is not finished: count it when the resumed turn ends.
    if (!parked) {
      MetricsLine("ai.handoff")
          .Add("src", state->handoff_local ? "local" : "server")
          .Add("tools", static_cast<int64_t>(state->turn_trace.tools_executed.size()))
          .Emit();
      state->handoff_turn = false;
    }
  }
  state->busy = false;
  state->iterations = 0;
  state->turn_scratch.clear();
  state->people_list_blocks.reset();
  state->pending_entry_id.clear();
  state->pending_thread_id.clear();
  state->assist_mode = AtAiMode::Local;
  state->turn_mode = AgentTurnMode::Conversation;
  PushLoading(state, false);
}

void AgentSession::CancelParkedApproval(const std::shared_ptr<Impl>& state, const ParkedApprovalState reason) {
  std::lock_guard lock(state->park_mutex);
  if (!state->parked_approval) {
    return;
  }
  state->parked_approval->state = reason;
  state->Log().info << "Parked approval " << state->parked_approval->id << " -> "
                    << ApprovalResumeErrorCode(reason == ParkedApprovalState::Cancelled
                                                   ? ApprovalResumeError::Superseded
                                                   : ApprovalResumeError::AlreadyResolved);
  state->parked_approval.reset();
}

ApprovalResumeError AgentSession::ValidateParkedResume(const std::optional<ParkedApproval>& park,
                                                       const std::string& approval_id, const bool busy) {
  if (busy) {
    return ApprovalResumeError::TurnBusy;
  }
  if (!park) {
    return ApprovalResumeError::NotFound;
  }
  if (park->id != approval_id) {
    return ApprovalResumeError::Superseded;
  }
  if (park->state == ParkedApprovalState::Resolved || park->state == ParkedApprovalState::Cancelled) {
    return ApprovalResumeError::AlreadyResolved;
  }
  if (park->state != ParkedApprovalState::Pending) {
    return ApprovalResumeError::AlreadyResolved;
  }
  constexpr int64_t kApprovalTtlMs = 30LL * 60LL * 1000LL;
  if (park->created_at_ms > 0 && util::NowUnixMs() - park->created_at_ms > kApprovalTtlMs) {
    return ApprovalResumeError::Expired;
  }
  return ApprovalResumeError::Ok;
}

void AgentSession::ParkForPermission(const std::shared_ptr<Impl>& state, TurnExecutionResult execution) {
  ParkedApproval park;
  park.id = util::GenerateUuid();
  park.thread_id = state->pending_thread_id;
  park.entry_id = state->pending_entry_id;
  park.plan = state->turn_plan;
  park.next_tool_index = execution.next_tool_index;
  park.scratch_so_far = state->turn_scratch;
  park.scratch_so_far.insert(park.scratch_so_far.end(), execution.scratch_append.begin(),
                             execution.scratch_append.end());
  park.tools_executed = execution.tools_executed;
  park.offered_tools = std::move(execution.offered_tools);
  park.state = ParkedApprovalState::Pending;
  park.created_at_ms = util::NowUnixMs();
  park.turn_mode = state->turn_mode;

  state->turn_trace.tools_executed = park.tools_executed;
  const std::string blocks = BuildToolPermissionChoiceBlocks(park.id, park.offered_tools);

  {
    std::lock_guard lock(state->park_mutex);
    state->parked_approval = std::move(park);
  }

  ValidateAndFinishAssistant(state, blocks, "stop", false);
}

void AgentSession::ResumeToolPermissionOnWorker(const std::shared_ptr<Impl>& state, std::string approval_id,
                                                std::string decision, std::string decision_label) {
  ParkedApproval park;
  {
    std::lock_guard lock(state->park_mutex);
    const ApprovalResumeError err =
        ValidateParkedResume(state->parked_approval, approval_id, state->busy.load());
    if (err != ApprovalResumeError::Ok) {
      PushError(state, std::string("Permission prompt inactive: ") + ApprovalResumeErrorCode(err));
      return;
    }
    park = *state->parked_approval;
    state->parked_approval->state = ParkedApprovalState::Executing;
  }

  if (state->busy.exchange(true)) {
    std::lock_guard lock(state->park_mutex);
    if (state->parked_approval && state->parked_approval->id == approval_id) {
      state->parked_approval->state = ParkedApprovalState::Pending;
    }
    PushError(state, std::string("Permission prompt inactive: ") +
                         ApprovalResumeErrorCode(ApprovalResumeError::TurnBusy));
    return;
  }

  state->cancelled = false;
  state->turn_mode = park.turn_mode;
  state->pending_thread_id = park.thread_id;
  state->turn_plan = park.plan;
  state->turn_scratch = park.scratch_so_far;
  state->people_list_blocks.reset();
  state->iterations = 0;
  PushLoading(state, true);

  auto append_decision_user = [&](const std::string& text) {
    if (state->turn_mode == AgentTurnMode::Conversation) {
      TranscriptEntry& entry = state->conversation.AppendUser(text, std::nullopt);
      state->pending_entry_id = entry.id;
      return;
    }
    if (state->turn_mode == AgentTurnMode::Thread && state->thread_store) {
      ThreadMessage user_message;
      user_message.id = util::GenerateUuid();
      user_message.thread_id = state->pending_thread_id;
      user_message.sender_contact_id = kLocalSelfContactId;
      user_message.text = text;
      user_message.timestamp = util::NowUnixMs();
      user_message.delivery = MessageDelivery::Local;
      user_message.transport = MessageTransport::Local;
      if (auto appended = state->thread_store->AppendMessage(user_message)) {
        state->pending_entry_id = appended->id;
      } else {
        state->pending_entry_id = user_message.id;
      }
      return;
    }
    state->pending_entry_id = util::GenerateUuid();
  };

  if (decision == "deny") {
    {
      std::lock_guard lock(state->park_mutex);
      state->parked_approval.reset();
    }
    append_decision_user(decision_label.empty() ? Tr("chat.permission.deny") : decision_label);
    ValidateAndFinishAssistant(state, BuildToolPermissionDeniedBlocks(park.offered_tools), "stop", false);
    return;
  }

  if (decision != "allow_once" && decision != "allow_always") {
    {
      std::lock_guard lock(state->park_mutex);
      if (state->parked_approval && state->parked_approval->id == approval_id) {
        state->parked_approval->state = ParkedApprovalState::Pending;
      }
    }
    FinishTurn(state);
    PushError(state, std::string("Permission prompt inactive: ") +
                         ApprovalResumeErrorCode(ApprovalResumeError::InvalidDecision));
    return;
  }

  if (decision == "allow_always") {
    for (const PlannedToolCall& tool : park.offered_tools) {
      ToolPermissionPolicy::SetToolDecision(state->tool_permissions, tool.name, "allow");
    }
    if (state->tool_permissions_saver) {
      if (auto saved = state->tool_permissions_saver(state->tool_permissions); !saved) {
        state->Log().warning << "Failed to persist tool permissions: " << saved.error().message;
      }
    }
  }

  std::unordered_set<std::string> session_grants;
  for (const PlannedToolCall& tool : park.offered_tools) {
    session_grants.insert(tool.name);
  }

  append_decision_user(decision_label.empty()
                           ? Tr(decision == "allow_always" ? "chat.permission.allow_always" : "chat.permission.allow_once")
                           : decision_label);

  const auto on_activity = [state](const std::string& tool_name, const std::string& status) {
    PushToolActivity(state, tool_name, status);
  };

  TurnExecutionOptions options;
  options.start_index = park.next_tool_index;
  options.permissions = state->tool_permissions;
  options.session_grants = std::move(session_grants);

  TurnExecutionResult execution = TurnExecutor::Execute(park.plan, state->tools, on_activity, options);
  {
    std::lock_guard lock(state->park_mutex);
    state->parked_approval.reset();
  }

  if (!execution.ok) {
    PushError(state, execution.error);
    FinishTurn(state);
    return;
  }
  if (execution.needs_permission) {
    PushError(state, "Tool permission still required after approval");
    FinishTurn(state);
    return;
  }

  state->turn_trace.tools_executed = park.tools_executed;
  state->turn_trace.tools_executed.insert(state->turn_trace.tools_executed.end(), execution.tools_executed.begin(),
                                          execution.tools_executed.end());
  state->turn_scratch.insert(state->turn_scratch.end(), execution.scratch_append.begin(),
                             execution.scratch_append.end());
  state->people_list_blocks = execution.people_list_blocks;
  ContinueAfterExecution(state);
}

std::vector<std::string> AgentSession::AllowedToolNames(const std::shared_ptr<Impl>& state) {
  std::vector<std::string> names;
  names.reserve(state->tools.Tools().size());
  for (const ToolDescriptor& tool : state->tools.Tools()) {
    names.push_back(tool.definition.name);
  }
  return names;
}

void AgentSession::PopulateTurnTraceFromPlan(const std::shared_ptr<Impl>& state) {
  state->turn_trace.plan_source = state->turn_plan.source;
  state->turn_trace.response_goal = state->turn_plan.response_goal;
  state->turn_trace.render_mode = state->turn_plan.render_mode;
  state->turn_trace.tools_planned.clear();
  for (const PlannedToolCall& call : state->turn_plan.tools) {
    state->turn_trace.tools_planned.push_back(call.name);
  }
}

void AgentSession::InjectSynthesisPolicy(const std::shared_ptr<Impl>& state) {
  const std::string policy = PromptBuilder::BuildSynthesisPrompt(state->turn_plan);
  if (!state->turn_scratch.empty() && state->turn_scratch.front().role == "system") {
    if (!state->turn_scratch.front().content.empty() && !policy.empty()) {
      state->turn_scratch.front().content += "\n\n";
    }
    state->turn_scratch.front().content += policy;
    return;
  }
  state->turn_scratch.insert(state->turn_scratch.begin(), ChatMessage{.role = "system", .content = policy});
}

void AgentSession::PersistAssistantToThread(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                            std::string* out_message_id) {
  if (!state->thread_store || state->pending_thread_id.empty()) {
    return;
  }
  if (state->turn_mode == AgentTurnMode::ScopedAssist && state->assist_mode != AtAiMode::Local) {
    // Shared assist reply is persisted by the chat UI after render; do not invent a store id here.
    return;
  }

  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = state->pending_thread_id;
  message.sender_contact_id = kAiAssistantContactId;
  message.text = StructuredTextParser::StorableText(assistant_raw);
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Local;
  message.relay_visible = state->turn_mode != AgentTurnMode::ScopedAssist;
  message.transport = MessageTransport::Local;
  auto appended = state->thread_store->AppendMessage(message);
  if (!appended) {
    state->Log().warning << "Failed to persist assistant message: " << appended.error().message;
    return;
  }
  if (out_message_id) {
    *out_message_id = appended->id.empty() ? message.id : appended->id;
  }
}

void AgentSession::FinishAssistantOutput(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                         const std::string& finish_reason) {
  if (state->turn_mode == AgentTurnMode::Conversation) {
    state->coordinator.CompleteTurn(state->conversation, state->pending_entry_id, assistant_raw);
    PushAssistantReady(state, state->pending_entry_id, assistant_raw, finish_reason);
    FinishTurn(state);
    return;
  }

  std::string assistant_message_id;
  const std::string thread_id = state->pending_thread_id;
  PersistAssistantToThread(state, assistant_raw, &assistant_message_id);
  PushAssistantReady(state, assistant_message_id, assistant_raw, finish_reason);
  if (state->turn_mode == AgentTurnMode::Thread && state->compaction && !thread_id.empty()) {
    state->compaction->MaybeCompactAsync(thread_id);
  }
  FinishTurn(state);
}

void AgentSession::ValidateAndFinishAssistant(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                              const std::string& finish_reason, const bool allow_repair) {
  const ParseResult parsed = StructuredTextParser::ParseFromLlmOutput(assistant_raw);
  state->turn_trace.parse_ok = parsed.ok;

  if (!parsed.ok && allow_repair && !state->turn_trace.output_repair_used) {
    RunOutputRepair(state, assistant_raw, parsed.error);
    return;
  }

  if (!parsed.ok) {
    state->Log().warning << "Assistant output parse failed: " << parsed.error;
  }

  FinishAssistantOutput(state, assistant_raw, finish_reason);
}

void AgentSession::RunOutputRepair(const std::shared_ptr<Impl>& state, const std::string& raw_output,
                                   const std::string& parse_error) {
  if (!state->llm) {
    FinishAssistantOutput(state, raw_output, "stop");
    return;
  }

  state->turn_trace.output_repair_used = true;
  const std::string repair_prompt =
      PromptBuilder::BuildOutputRepairPrompt(state->turn_plan, raw_output, parse_error);

  ChatCompletionRequest request;
  request.messages = state->turn_scratch;
  request.messages.push_back(ChatMessage{.role = "user", .content = repair_prompt});

  auto result = state->llm->Complete(request);
  if (!result || !result->content || result->content->empty()) {
    FinishAssistantOutput(state, raw_output, "stop");
    return;
  }

  ValidateAndFinishAssistant(state, *result->content, result->finish_reason.empty() ? "stop" : result->finish_reason,
                             false);
}

void AgentSession::DispatchRefinementToolCalls(const std::shared_ptr<Impl>& state,
                                               const std::vector<ToolCall>& tool_calls,
                                               const std::string& assistant_content) {
  state->turn_trace.refinement_used = true;

  ChatMessage assistant_message;
  assistant_message.role = "assistant";
  assistant_message.content = assistant_content;
  assistant_message.tool_calls = ToolCallsToJson(tool_calls);
  state->turn_scratch.push_back(std::move(assistant_message));

  AppRuntime::PostWorkerNormal([state, tool_calls]() {
    if (state->cancelled) {
      AgentSession::FinishTurn(state);
      return;
    }

    for (const ToolCall& call : tool_calls) {
      AgentSession::PushToolActivity(state, call.name, "running");

      ToolMeta meta;
      for (const ToolDescriptor& tool : state->tools.Tools()) {
        if (tool.definition.name == call.name) {
          meta = tool.meta;
          break;
        }
      }
      const ToolPermissionEval eval =
          ToolPermissionPolicy::Evaluate(call.name, meta, state->tool_permissions, {});
      if (eval.verdict == ToolPermissionVerdict::Ask || eval.verdict == ToolPermissionVerdict::Deny) {
        AgentSession::PushToolActivity(state, call.name, "error");
        state->turn_trace.tools_executed.push_back(call.name);
        ChatMessage tool_message;
        tool_message.role = "tool";
        tool_message.tool_call_id = call.id;
        tool_message.content =
            eval.verdict == ToolPermissionVerdict::Deny
                ? "Tool error: blocked by tool permission settings."
                : "Tool error: permission required — ask the user to confirm this action in a follow-up.";
        state->turn_scratch.push_back(std::move(tool_message));
        continue;
      }

      auto result = state->tools.Execute(call.name, call.arguments);
      AgentSession::PushToolActivity(state, call.name, result ? "done" : "error");
      state->turn_trace.tools_executed.push_back(call.name);

      ChatMessage tool_message;
      tool_message.role = "tool";
      tool_message.tool_call_id = call.id;
      if (result) {
        tool_message.content = FormatToolResultForLlm(call.name, *result);
      } else {
        tool_message.content = "Tool error: " + result.error().message;
      }
      state->turn_scratch.push_back(std::move(tool_message));
    }

    AppendSynthesisReminder(state->turn_scratch, state->turn_plan);

    ++state->iterations;
    if (state->iterations >= kMaxIterations) {
      AgentSession::PushError(state, "Agent iteration limit reached");
      AgentSession::FinishTurn(state);
      return;
    }

    AgentSession::RunSynthesisStep(state);
  });
}

void AgentSession::HandleSynthesisResponse(const std::shared_ptr<Impl>& state,
                                           const ChatCompletionResponse& response) {
  if (state->cancelled) {
    FinishTurn(state);
    return;
  }

  state->turn_trace.synthesis_ms = ElapsedMs(state->synthesis_started);

  if (!response.tool_calls.empty()) {
    DispatchRefinementToolCalls(state, response.tool_calls, response.content.value_or(""));
    return;
  }

  if (response.content) {
    if (auto embedded = StructuredTextParser::ExtractEmbeddedToolCalls(*response.content)) {
      state->Log().warning << "Synthesis returned embedded tool blocks; extracting";
      std::vector<ToolCall> tool_calls;
      tool_calls.reserve(embedded->size());
      for (size_t i = 0; i < embedded->size(); ++i) {
        tool_calls.push_back(ToolCall{
            .id = "embedded_" + std::to_string(i + 1),
            .name = (*embedded)[i].name,
            .arguments = (*embedded)[i].arguments,
        });
      }
      DispatchRefinementToolCalls(state, tool_calls, *response.content);
      return;
    }
  }

  if (!response.content || response.content->empty()) {
    PushError(state, "LLM response missing content");
    FinishTurn(state);
    return;
  }

  ValidateAndFinishAssistant(state, *response.content, response.finish_reason, true);
}

void AgentSession::RunSynthesisStep(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm) {
    FinishTurn(state);
    return;
  }

  state->synthesis_started = std::chrono::steady_clock::now();

  ChatCompletionRequest request;
  request.messages = state->turn_scratch;
  request.tools = state->tools.Definitions();

  auto result = state->llm->Complete(request);
  if (!result) {
    PushError(state, result.error());
    FinishTurn(state);
    return;
  }

  HandleSynthesisResponse(state, *result);
}

void AgentSession::ContinueAfterExecution(const std::shared_ptr<Impl>& state) {
  if (state->people_list_blocks) {
    ValidateAndFinishAssistant(state, *state->people_list_blocks, "stop", false);
    return;
  }

  InjectSynthesisPolicy(state);
  RunSynthesisStep(state);
}

Roe<TurnPlan> AgentSession::ResolveTurnPlan(const std::shared_ptr<Impl>& state) {
  if (state->pending_user_payload && !state->pending_user_payload->empty()) {
    if (auto payload_plan = TryBuildPlanFromPayload(state->pending_user_text, *state->pending_user_payload)) {
      auto validated = ValidateTurnPlan(*payload_plan, AllowedToolNames(state));
      if (validated) {
        return validated;
      }
      if (payload_plan->response_goal == ResponseGoal::DisplayFeed) {
        // The feed tool comes from the promoted MCP server; without it there is nothing to show, and asking the
        // model instead would only produce a confusing answer.
        Error unavailable = validated.error();
        unavailable.user = TrOrDefault("feed.unavailable", "Articles are unavailable right now.");
        return unavailable;
      }
    }
  }

  if (!state->llm) {
    return AppError::Config(Err::Config::MissingKey, "LLM not configured");
  }

  state->planner_started = std::chrono::steady_clock::now();
  auto plan = TurnPlanner::Plan(*state->llm, state->turn_scratch, state->tools.SummaryForPrompt(),
                                AllowedToolNames(state), state->pending_user_text);
  if (plan) {
    state->turn_trace.planner_ms = ElapsedMs(state->planner_started);
  }
  return plan;
}

void AgentSession::RunTurnPipeline(const std::shared_ptr<Impl>& state) {
  state->turn_trace = TurnTrace{};
  state->turn_trace.turn_id = util::GenerateUuid();
  state->turn_trace.entry_id = state->pending_entry_id;
  state->turn_trace.thread_id = state->pending_thread_id;

  auto plan = ResolveTurnPlan(state);
  if (!plan) {
    PushError(state, plan.error());
    FinishTurn(state);
    return;
  }

  state->turn_plan = *plan;
  if (state->turn_plan.user_request.empty()) {
    state->turn_plan.user_request = state->pending_user_text;
  }
  PopulateTurnTraceFromPlan(state);

  const auto on_activity = [state](const std::string& tool_name, const std::string& status) {
    AgentSession::PushToolActivity(state, tool_name, status);
  };

  TurnExecutionOptions options;
  options.permissions = state->tool_permissions;
  TurnExecutionResult execution = TurnExecutor::Execute(state->turn_plan, state->tools, on_activity, options);
  if (!execution.ok) {
    PushError(state, execution.error);
    FinishTurn(state);
    return;
  }
  if (execution.needs_permission) {
    ParkForPermission(state, std::move(execution));
    return;
  }

  state->turn_trace.tools_executed = execution.tools_executed;
  state->turn_scratch.insert(state->turn_scratch.end(), execution.scratch_append.begin(), execution.scratch_append.end());
  state->people_list_blocks = execution.people_list_blocks;

  ContinueAfterExecution(state);
}

bool AgentSession::UseBriefStream(const std::shared_ptr<Impl>& state) {
  // A scoped assist ("@ai" in a chat) decides for itself in StartTurn: only its local mode streams.
  return state->turn_mode != AgentTurnMode::ScopedAssist && BriefStreamAvailable(state);
}

bool AgentSession::BriefStreamAvailable(const std::shared_ptr<Impl>& state) {
  if (state->pending_user_payload && !state->pending_user_payload->empty()) {
    return false;
  }
  if (!state->brief_stream && !state->brief_stream_override) {
    return false;
  }
  return ResolvePreset(state->config) == "brief";
}

std::vector<std::string> AgentSession::AppActionCapabilities(const std::shared_ptr<Impl>& state) {
  std::vector<std::string> names;
  for (const ToolDescriptor& tool : state->tools.Tools()) {
    if (tool.meta.provider == "messaging" || tool.meta.provider == "settings") {
      names.push_back(tool.definition.name);
    }
  }
  return names;
}

bool AgentSession::TryLocalAppAction(const std::shared_ptr<Impl>& state) {
  if (state->pending_image) {
    return false;
  }
  const auto tool = ClassifyAppAction(state->pending_user_text, AppActionCapabilities(state));
  if (!tool) {
    return false;
  }
  state->Log().info << "Turn judged an app action on the device (" << *tool << "); running the local pipeline";
  state->handoff_turn = true;
  state->handoff_local = true;
  RunTurnPipeline(state);
  return true;
}

void AgentSession::EmitStreamedAnswer(const std::shared_ptr<Impl>& state, const std::string& text,
                                      const std::string& finish_reason, const std::vector<BriefAiSource>& sources) {
  state->turn_plan = TurnPlan{};
  state->turn_plan.render_mode = RenderMode::Markdown;

  if (state->turn_mode == AgentTurnMode::Conversation) {
    state->coordinator.CompleteTurn(state->conversation, state->pending_entry_id, text);
    PushAssistantReady(state, state->pending_entry_id, text, finish_reason, sources);
    return;
  }

  std::string assistant_message_id;
  const std::string thread_id = state->pending_thread_id;
  PersistAssistantToThread(state, text, &assistant_message_id);
  PushAssistantReady(state, assistant_message_id, text, finish_reason, sources);
  if (state->turn_mode == AgentTurnMode::Thread && state->compaction && !thread_id.empty()) {
    state->compaction->MaybeCompactAsync(thread_id);
  }
}

void AgentSession::StreamBriefTurn(const std::shared_ptr<Impl>& state, std::vector<BriefAiHistoryTurn> history,
                                   std::string summary) {
  // Same gate as LlmClient::Complete: no key means a settings problem, not a network one — say so
  // without a round-trip and without offering a retry.
  if (state->config.llm.api_key.empty()) {
    PushError(state, AppError::Config(Err::Config::MissingKey, "LLM API key not configured"));
    FinishTurn(state);
    return;
  }
  state->turn_trace = TurnTrace{};
  state->turn_trace.turn_id = util::GenerateUuid();
  state->turn_trace.entry_id = state->pending_entry_id;
  state->turn_trace.thread_id = state->pending_thread_id;

  BriefAiRequest request;
  request.message = state->pending_user_text;
  request.history = std::move(history); // the client keeps the last 6
  request.summary = std::move(summary);
  request.app_version = AppVersionString();
  if (state->pending_image) {
    request.image = state->pending_image->image;
  }
  // A turn with an image is never handed back, so it advertises no capabilities; neither does "@ai" in a
  // chat (contract: only the question goes out).
  if (!request.image && state->turn_mode != AgentTurnMode::ScopedAssist) {
    request.capabilities = AppActionCapabilities(state);
  }

  // This turn's cancel flag and generation. Cancel() + a new Submit replace both; a stream that is
  // still draining then sees a stale generation and must not touch the new turn's state.
  std::shared_ptr<std::atomic<bool>> cancel;
  {
    std::lock_guard lock(state->stream_cancel_mutex);
    cancel = state->stream_cancel;
  }
  const uint64_t generation = state->turn_generation.load(std::memory_order_acquire);
  const auto current = [state, generation]() {
    return state->turn_generation.load(std::memory_order_acquire) == generation;
  };

  // Operational metrics only: timings and outcome, never text.
  struct TurnStats {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> first_token;
    size_t deltas = 0;
    std::string route;
  };
  auto stats = std::make_shared<TurnStats>();
  // Size bucket only: no name, dimensions or content.
  const bool has_image = request.image.has_value();
  const char* image_kb = !has_image ? "none" : request.image->data.size() < 500u * 1024u ? "<500"
                         : request.image->data.size() < 1536u * 1024u              ? "<1536"
                                                                                   : ">=1536";
  const auto emit_turn = [stats, has_image, image_kb](const char* outcome) {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = [](auto from, auto to) -> int64_t {
      return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
    };
    MetricsLine("ai.turn")
        .Add("outcome", outcome)
        .Add("route", stats->route.empty() ? "none" : stats->route)
        .Add("first_token_ms", stats->first_token ? ms(stats->started, *stats->first_token) : int64_t{-1})
        .Add("total_ms", ms(stats->started, now))
        .Add("deltas", static_cast<int64_t>(stats->deltas))
        .Add("image", static_cast<int64_t>(has_image ? 1 : 0))
        .Add("image_kb", image_kb)
        .Emit();
  };

  // Image turn only: which HTTP status (413 / 400) ended the request, so the GUI can word it itself.
  auto image_error_kind = std::make_shared<std::string>();

  BriefTurnSinks sinks;
  // Deltas carry the whole text so far; cap them at 20/s so a long answer does not flood the UI queue.
  auto last_delta = std::make_shared<std::optional<std::chrono::steady_clock::time_point>>();
  sinks.on_delta = [state, current, last_delta, stats](const std::string& text) {
    const auto now = std::chrono::steady_clock::now();
    if (!stats->first_token) {
      stats->first_token = now;
    }
    ++stats->deltas;
    if (!current() || (*last_delta && now - **last_delta < std::chrono::milliseconds(50))) {
      return;
    }
    *last_delta = now;
    PushEvent(state, AgentEvent{.type = AgentEventType::AssistantDelta,
                                .text = text,
                                .entry_id = state->pending_entry_id,
                                .thread_id = state->pending_thread_id});
  };
  sinks.on_status = [state, current](const std::string& tool, const std::string& phase, const std::string& /*query*/) {
    if (current()) {
      PushToolActivity(state, tool, phase);
    }
  };
  sinks.on_meta = [stats](const std::string& route) { stats->route = route; };
  sinks.on_done = [state, current, emit_turn](const std::string& response, const std::string& finish,
                                              const std::vector<BriefAiSource>& sources) {
    emit_turn(finish == "length" ? "done_truncated" : "done");
    if (!current()) {
      state->Log().info << "brief_AI stream finished after the turn was replaced; dropped";
      return;
    }
    EmitStreamedAnswer(state, response, finish, sources);
    FinishTurn(state);
  };
  sinks.on_handoff = [state, current, emit_turn, has_image]() {
    emit_turn("handoff");
    if (!current()) {
      return;
    }
    if (has_image) {
      // The contract never hands back a turn with an image; do not run the local pipeline on one.
      state->Log().warning << "brief_AI handed back a turn with an image; dropped";
      PushError(state, "The AI could not answer this image. Try again.");
      FinishTurn(state);
      return;
    }
    state->Log().info << "brief_AI handed the turn back; running the local pipeline";
    state->handoff_turn = true;
    state->handoff_local = false;
    RunTurnPipeline(state);
  };
  sinks.on_error = [state, current, emit_turn, image_error_kind](const std::string& message, const bool retryable,
                                                                 const std::string& partial) {
    state->Log().warning << "brief_AI stream error (retryable=" << retryable << ", partial_chars=" << partial.size() << ")";
    emit_turn(partial.empty() ? "error" : "error_partial");
    if (!current()) {
      return;
    }
    if (!partial.empty()) {
      EmitStreamedAnswer(state, partial, "error", {});
    }
    PushError(state, message, retryable, *image_error_kind);
    FinishTurn(state);
  };
  sinks.on_cancelled = [state, current, emit_turn](const std::string& partial) {
    emit_turn("cancelled");
    if (!current()) {
      return; // Cancel() followed by a new Submit: the new turn owns the state now
    }
    if (!partial.empty()) {
      EmitStreamedAnswer(state, partial, "cancelled", {});
    }
    FinishTurn(state);
  };

  BriefAiStreamFn stream = state->brief_stream_override ? state->brief_stream_override : state->brief_stream;
  if (has_image) {
    // The failed status is only in the technical detail ("LLM HTTP 413: ..."); note it for the error sink.
    stream = [inner = std::move(stream), image_error_kind](const BriefAiRequest& req,
                                                           const std::function<void(const BriefAiEvent&)>& on_event,
                                                           const std::atomic<bool>& cancelled) -> Roe<BriefAiOutcome> {
      Roe<BriefAiOutcome> outcome = inner(req, on_event, cancelled);
      if (!outcome) {
        const std::string& detail = outcome.error().message;
        if (detail.find("LLM HTTP 413") != std::string::npos) {
          *image_error_kind = "image_too_large";
        } else if (detail.find("LLM HTTP 400") != std::string::npos) {
          *image_error_kind = "image_unsupported";
        }
      }
      return outcome;
    };
  }
  BriefTurnRunner::Run(stream, request, *cancel, sinks);
}

void AgentSession::StartTurn(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm) {
    FinishTurn(state);
    return;
  }

  CancelParkedApproval(state, ParkedApprovalState::Cancelled);

  state->iterations = 0;
  state->turn_scratch.clear();
  state->people_list_blocks.reset();
  PushLoading(state, true);

  if (state->turn_mode == AgentTurnMode::Thread) {
    if (!state->thread_store) {
      PushError(state, "Thread store not configured");
      FinishTurn(state);
      return;
    }

    if (state->pending_image && !UseBriefStream(state)) {
      // Only the streamed brief path carries an image; never drop it silently.
      PushError(state, "Images can only be sent with the Brief AI model.");
      FinishTurn(state);
      return;
    }

    ThreadMessage user_message;
    user_message.id = state->pending_image && !state->pending_image->message_id.empty()
                          ? state->pending_image->message_id
                          : util::GenerateUuid();
    user_message.thread_id = state->pending_thread_id;
    user_message.sender_contact_id = kLocalSelfContactId;
    // Stored as text only: later history reads "[Image] <question>"; the bytes are never kept here.
    user_message.text = (state->pending_image ? std::string(kAiImageTurnMarker) : std::string()) + state->pending_user_text;
    user_message.timestamp = util::NowUnixMs();
    user_message.delivery = MessageDelivery::Local;
    user_message.transport = MessageTransport::Local;
    if (auto appended = state->thread_store->AppendMessage(user_message)) {
      state->pending_entry_id = appended->id;
    } else {
      state->pending_entry_id = user_message.id;
    }

    auto messages = state->thread_store->GetMessagesForContext(state->pending_thread_id, state->config.context);
    if (!messages) {
      PushError(state, messages.error());
      FinishTurn(state);
      return;
    }

    std::optional<ConversationSummary> summary;
    if (auto memory = state->thread_store->GetThreadMemory(state->pending_thread_id)) {
      summary = *memory;
    }

    ThreadContextPolicy policy(state->config.context);
    const std::string system_prompt = PromptBuilder::BuildChatAgentSystemPrompt(state->tools.SummaryForPrompt(), ResolvePreset(state->config) == "brief");
    const ContextBuildResult built = policy.Build(*messages, system_prompt, state->pending_user_text,
                                                  state->pending_user_payload, summary);
    state->turn_scratch = built.messages;
    if (UseBriefStream(state)) {
      if (TryLocalAppAction(state)) {
        return;
      }
      // Built context is a transcript blob plus a pending user turn; send the real messages instead.
      std::vector<BriefAiHistoryTurn> history;
      for (const ThreadMessage& message : *messages) {
        if (message.id == state->pending_entry_id || message.text.empty()) {
          continue;
        }
        const bool assistant = message.sender_contact_id == kAiAssistantContactId;
        // Local-pipeline answers are stored as UI block documents; the backend gets their prose, and a
        // document with nothing readable (buttons, a form) is left out rather than sent as JSON.
        std::string content = message.text;
        if (assistant) {
          if (const auto prose = StructuredTextParser::PlainTextIfBlocks(message.text)) {
            if (prose->empty()) {
              continue;
            }
            content = *prose;
          }
        }
        history.push_back(BriefAiHistoryTurn{.role = assistant ? "assistant" : "user", .content = std::move(content)});
      }
      StreamBriefTurn(state, std::move(history), summary ? summary->text : std::string());
      return;
    }
    RunTurnPipeline(state);
    return;
  }

  if (state->turn_mode == AgentTurnMode::ScopedAssist) {
    if (!state->thread_store) {
      PushError(state, "Thread store not configured");
      FinishTurn(state);
      return;
    }

    state->pending_entry_id = util::GenerateUuid();

    // Only the question goes out; the chat transcript stays on the device (plan decision 11).
    ThreadContextPolicy policy(state->config.context);
    state->turn_scratch = policy.BuildAssistContext(state->pending_user_text);
    if (!state->turn_scratch.empty() && state->turn_scratch.front().role == "system") {
      state->turn_scratch.front().content =
          PromptBuilder::BuildScopedAssistSystemPrompt(state->tools.SummaryForPrompt(), ResolvePreset(state->config) == "brief");
    }
    // With the brief preset a local "@ai" is a brief_AI question like any other, so it gets the current
    // date, retrieval and sources (the bare model answered from its training data: wrong year, wrong
    // language). No history is sent. A shared reply keeps the local path: the chat UI renders and relays it.
    if (state->assist_mode == AtAiMode::Local && BriefStreamAvailable(state)) {
      StreamBriefTurn(state, {}, std::string());
      return;
    }
    RunTurnPipeline(state);
    return;
  }

  TranscriptEntry& entry = state->conversation.AppendUser(state->pending_user_text, state->pending_user_payload);
  state->pending_entry_id = entry.id;

  const std::string system_prompt = PromptBuilder::BuildChatAgentSystemPrompt(state->tools.SummaryForPrompt(), ResolvePreset(state->config) == "brief");
  const TurnSnapshot snapshot =
      state->coordinator.BeginTurn(state->conversation, system_prompt, entry, state->config.context);
  state->turn_scratch = snapshot.messages;
  if (UseBriefStream(state)) {
    if (TryLocalAppAction(state)) {
      return;
    }
    // Context layout: [system, history..., pending user]; the policy appends the pending user message last.
    std::vector<BriefAiHistoryTurn> history;
    for (size_t i = 1; i + 1 < snapshot.messages.size(); ++i) {
      history.push_back(BriefAiHistoryTurn{.role = snapshot.messages[i].role, .content = snapshot.messages[i].content});
    }
    const auto& summary = state->conversation.Summary();
    StreamBriefTurn(state, std::move(history), summary ? summary->text : std::string());
    return;
  }
  RunTurnPipeline(state);
}

void AgentSession::RefreshCompactionService(const std::shared_ptr<Impl>& state) {
  if (state->thread_store && state->llm) {
    state->compaction = std::make_unique<ThreadCompactionService>(*state->thread_store, *state->thread_store, *state->thread_store, state->llm.get());
  } else {
    state->compaction.reset();
  }
}

void AgentSession::ConfigureOnIO(const std::shared_ptr<Impl>& state) {
  // Configure() counted this run and releases it when the posted closure is destroyed.
  // Serialize ConfigureOnIO body — concurrent Configure() raced mcp.Start/Stop (UAF → SEGV in logger).
  // Cancel() means "abort the active turn", not "abort agent configure". OnNewChat /
  // find-someone calls Cancel while Me→ReloadFromDisk may still be reconfiguring; tearing
  // down llm here left the session unconfigured and the next send showed MissingKey.
  // Do not PauseBackgroundWork here: this already runs on a worker pool thread. Pausing blocked
  // concurrent posts (profile unlock, inbox) until Resume — and a missed Resume left
  // unlock_in_progress stuck on Android.
  std::lock_guard work_lock(state->configure_work_mutex);
  try {
    LlmConfig llm_config = state->config.llm;
    if (ResolvePreset(state->config) == "brief") {
      llm_config.require_api_key = true;
    }
    state->llm = std::make_unique<LlmClient>(llm_config);
    // Streamed answers go through the gateway's /pp/chat/stream (live since app-static-api #169);
    // PP_BROWSER_BRIEF_STREAM_URL only redirects them to a local fake server for development.
    {
      const auto brief_client = std::make_shared<BriefAiClient>(llm_config, BriefStreamUrlOverride());
      state->brief_stream = [brief_client](const BriefAiRequest& request,
                                           const std::function<void(const BriefAiEvent&)>& on_event,
                                           const std::atomic<bool>& cancel) {
        return brief_client->Stream(request, on_event, cancel);
      };
    }

    const AppConfig defaults = Config::DefaultAppConfig();
    McpClient::SetHttpPost([](const std::string& url, const std::string& body,
                                  const std::map<std::string, std::string>& headers) {
      return HttpClient::Post(url, body, headers);
    });
    state->mcp.Start(state->config, defaults);

    std::vector<std::string> custom_prefixes;
    custom_prefixes.reserve(state->config.mcp_servers.size());
    for (const McpConfig& entry : state->config.mcp_servers) {
      custom_prefixes.push_back(entry.id);
    }

    BuildToolRegistryFromConfig(state->tools, state->config, state->mcp.PromotedPtr(), state->mcp.CustomPtrs(),
                                custom_prefixes);
    if (state->tool_registration_hook) {
      state->tool_registration_hook(state->tools);
    }
    state->configured = true;
    RefreshCompactionService(state);

    state->Log().info << "Agent configured with " << state->tools.Tools().size() << " tool(s)";
    for (const ToolDescriptor& tool : state->tools.Tools()) {
      state->Log().info << "  - " << tool.definition.name;
    }
  } catch (const std::exception& e) {
    state->configured = false;
    state->mcp.Stop();
    state->llm.reset();
    state->tools.Clear();
    RefreshCompactionService(state);
    state->Log().error << "Agent configure failed: " << e.what();
    PushError(state, std::string("Agent configure failed: ") + e.what());
    return;
  } catch (...) {
    state->configured = false;
    state->mcp.Stop();
    state->llm.reset();
    state->tools.Clear();
    RefreshCompactionService(state);
    state->Log().error << "Agent configure failed: unknown exception";
    PushError(state, "Agent configure failed");
    return;
  }

  if (state->submit_when_ready && !state->pending_user_text.empty()) {
    state->submit_when_ready = false;
    if (!state->cancelled) {
      StartTurn(state);
    } else {
      // OnNewChat Cancel raced configure; do not leave busy stuck from Submit.
      state->busy = false;
    }
  }
}

AgentSession::AgentSession() : impl_(std::make_shared<Impl>()) {
  impl_->conversation.StartNewConversation();
}

AgentSession::~AgentSession() {
  Cancel();
}

void AgentSession::Configure(const AppConfig& config) {
  impl_->config = config;
  impl_->configured = false;
  impl_->cancelled = false;

  // Count the configure as in flight before it is posted, so WaitForConfigureIdle() called right
  // after Configure() cannot return before the worker has even started. The count is released when
  // the posted closure is destroyed — after it ran, or when the runtime drops it during teardown —
  // so a dropped post cannot leave WaitForConfigureIdle() waiting forever.
  struct InflightToken {
    explicit InflightToken(std::shared_ptr<Impl> owner) : impl(std::move(owner)) {}
    InflightToken(const InflightToken&) = delete;
    InflightToken& operator=(const InflightToken&) = delete;
    ~InflightToken() {
      std::lock_guard lock(impl->configure_mutex);
      --impl->configure_inflight;
      impl->configure_cv.notify_all();
    }
    std::shared_ptr<Impl> impl;
  };
  {
    std::lock_guard lock(impl_->configure_mutex);
    ++impl_->configure_inflight;
  }
  AppRuntime::PostWorkerNormal(
      [impl = impl_, token = std::make_shared<InflightToken>(impl_)]() { ConfigureOnIO(impl); });
}

void AgentSession::SetToolRegistrationHook(ToolRegistrationHook hook) {
  impl_->tool_registration_hook = std::move(hook);
}

void AgentSession::SetBriefAiStream(BriefAiStreamFn fn) {
  impl_->brief_stream_override = std::move(fn);
}

void AgentSession::SetToolPermissions(ToolPermissionsPrefs permissions) {
  impl_->tool_permissions = std::move(permissions);
}

void AgentSession::SetToolPermissionsSaver(ToolPermissionsSaveFn saver) {
  impl_->tool_permissions_saver = std::move(saver);
}

McpClient* AgentSession::PromotedMcp() {
  return impl_->mcp.PromotedPtr();
}

Roe<void> AgentSession::ResumeToolPermission(const std::string& approval_id, const std::string& decision,
                                             const std::string& decision_label) {
  if (approval_id.empty()) {
    return Error(ApprovalResumeErrorCode(ApprovalResumeError::NotFound));
  }
  if (decision != "allow_once" && decision != "allow_always" && decision != "deny") {
    return Error(ApprovalResumeErrorCode(ApprovalResumeError::InvalidDecision));
  }

  {
    std::lock_guard lock(impl_->park_mutex);
    const ApprovalResumeError err =
        ValidateParkedResume(impl_->parked_approval, approval_id, impl_->busy.load());
    if (err != ApprovalResumeError::Ok) {
      return Error(ApprovalResumeErrorCode(err));
    }
  }

  AppRuntime::PostWorkerNormal(
      [impl = impl_, approval_id, decision, decision_label]() {
        ResumeToolPermissionOnWorker(impl, approval_id, decision, decision_label);
      });
  return {};
}

bool AgentSession::IsConfigured() const {
  return impl_->configured;
}

const Conversation& AgentSession::conversation() const {
  return impl_->conversation;
}

TranscriptEntry& AgentSession::AppendUserMessage(const std::string& user_text,
                                                  std::optional<std::string> user_payload) {
  return impl_->conversation.AppendUser(user_text, std::move(user_payload));
}

bool AgentSession::CompleteAssistantMessage(const std::string& entry_id, const std::string& assistant_raw) {
  return impl_->conversation.CompleteTurn(entry_id, assistant_raw);
}

bool AgentSession::SetAssistantDisplay(const std::string& entry_id, const std::string& assistant_rml,
                                       std::vector<TranscriptChatAction> chat_actions) {
  return impl_->conversation.SetAssistantDisplay(entry_id, assistant_rml, std::move(chat_actions));
}

void AgentSession::SetThreadStore(IThreadStore* store) {
  impl_->thread_store = store;
  RefreshCompactionService(impl_);
}

void AgentSession::BeginStreamTurn() {
  std::lock_guard lock(impl_->stream_cancel_mutex);
  impl_->stream_cancel = std::make_shared<std::atomic<bool>>(false);
  impl_->turn_generation.fetch_add(1, std::memory_order_acq_rel);
}

void AgentSession::Submit(const std::string& user_text, std::optional<std::string> user_payload) {
  if (user_text.empty() || impl_->busy.exchange(true)) {
    return;
  }

  impl_->cancelled = false;
  BeginStreamTurn();
  impl_->pending_user_text = user_text;
  impl_->pending_user_payload = std::move(user_payload);
  impl_->pending_image.reset();
  impl_->turn_mode = AgentTurnMode::Conversation;
  impl_->pending_thread_id.clear();

  AppRuntime::PostWorkerNormal([impl = impl_]() {
    if (!impl->configured) {
      impl->submit_when_ready = true;
      return;
    }
    StartTurn(impl);
  });
}

void AgentSession::SubmitToThread(const std::string& thread_id, const std::string& user_text,
                                  std::optional<std::string> user_payload, std::optional<AgentImageTurn> image) {
  if (user_text.empty() || impl_->busy.exchange(true)) {
    return;
  }

  impl_->cancelled = false;
  BeginStreamTurn();
  impl_->pending_user_text = user_text;
  impl_->pending_user_payload = std::move(user_payload);
  impl_->pending_image = std::move(image);
  impl_->pending_thread_id = thread_id;
  impl_->turn_mode = AgentTurnMode::Thread;

  AppRuntime::PostWorkerNormal([impl = impl_]() {
    if (!impl->configured) {
      impl->submit_when_ready = true;
      return;
    }
    StartTurn(impl);
  });
}

void AgentSession::SubmitScopedAssist(const std::string& thread_id, const std::string& prompt,
                                      std::optional<std::string> user_payload, const AtAiMode mode) {
  if (prompt.empty() || impl_->busy.exchange(true)) {
    return;
  }

  impl_->cancelled = false;
  BeginStreamTurn();
  impl_->pending_user_text = prompt;
  impl_->pending_user_payload = std::move(user_payload);
  impl_->pending_image.reset();
  impl_->pending_thread_id = thread_id;
  impl_->turn_mode = AgentTurnMode::ScopedAssist;
  impl_->assist_mode = mode;

  AppRuntime::PostWorkerNormal([impl = impl_]() {
    if (!impl->configured) {
      impl->submit_when_ready = true;
      return;
    }
    StartTurn(impl);
  });
}

void AgentSession::PollEvents(std::vector<AgentEvent>& out) {
  std::lock_guard lock(impl_->event_mutex);
  out.insert(out.end(), impl_->pending_events.begin(), impl_->pending_events.end());
  impl_->pending_events.clear();
}

void AgentSession::Cancel() {
  impl_->cancelled = true;
  {
    std::lock_guard lock(impl_->stream_cancel_mutex);
    impl_->stream_cancel->store(true);
  }
  impl_->busy = false;
  impl_->tool_registration_hook = nullptr;
  CancelParkedApproval(impl_, ParkedApprovalState::Cancelled);
}

void AgentSession::WaitForConfigureIdle() {
  std::unique_lock lock(impl_->configure_mutex);
  impl_->configure_cv.wait(lock, [&] { return impl_->configure_inflight == 0; });
}

void AgentSession::StartNewConversation() {
  AppRuntime::PostWorkerNormal([impl = impl_]() {
    CancelParkedApproval(impl, ParkedApprovalState::Cancelled);
    impl->conversation.StartNewConversation();
    impl->turn_scratch.clear();
    impl->pending_entry_id.clear();
    impl->pending_user_text.clear();
    impl->pending_user_payload.reset();
    impl->pending_image.reset();
    impl->cancelled = false;
    impl->busy = false;
  });
}

} // namespace pbr
