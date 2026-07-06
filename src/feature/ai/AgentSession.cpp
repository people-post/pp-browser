#include "feature/ai/AgentSession.h"
#include "base/platform/Platform.h"

#include "feature/ai/PayloadTurnPlanBuilder.h"
#include "base/ai/PromptBuilder.h"
#include "base/ai/StructuredTextParser.h"
#include "feature/ai/ToolRegistry.h"
#include "base/ai/ToolResultFormatter.h"
#include "feature/ai/TurnExecutor.h"
#include "feature/ai/TurnPlanner.h"
#include "base/ai/conversation/Conversation.h"
#include "base/ai/conversation/ThreadCompactionService.h"
#include "base/ai/conversation/ThreadContextPolicy.h"
#include "base/ai/conversation/TurnCoordinator.h"
#include "common/Logger.h"
#include "base/ai/mcp/McpClient.h"
#include "base/ai/mcp/McpRuntime.h"
#include "base/data/SessionStore.h"
#include "common/Utilities.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/platform/BrowserThread.h"
#include "feature/messaging/MessagingHub.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pbr {

enum class AgentTurnMode { Conversation, Thread, ScopedAssist };

namespace {

constexpr int kMaxIterations = 8;

nlohmann::json ToolCallsToJson(const std::vector<ToolCall>& tool_calls) {
  nlohmann::json out = nlohmann::json::array();
  for (const ToolCall& call : tool_calls) {
    out.push_back({{"id", call.id},
                   {"type", "function"},
                   {"function", {{"name", call.name}, {"arguments", call.arguments.dump()}}}});
  }
  return out;
}

void AppendSynthesisReminder(std::vector<ChatMessage>& messages, const TurnPlan& plan) {
  if (messages.empty() || messages.back().role != "tool") {
    return;
  }
  messages.back().content += "\n\n" + PromptBuilder::BuildSynthesisRefinementReminder(plan);
}

} // namespace

struct AgentSession::Impl {
  std::mutex event_mutex;
  std::vector<AgentEvent> pending_events;

  std::atomic<bool> cancelled{false};
  std::atomic<bool> busy{false};

  AppConfig config;
  std::unique_ptr<LlmClient> llm;
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
};

void AgentSession::PushEvent(const std::shared_ptr<Impl>& state, AgentEvent event) {
  BrowserThread::PostTask(BrowserThreadId::UI, [state, event = std::move(event)]() mutable {
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
                                      const std::string& text, const std::string& finish_reason) {
  PushEvent(state, AgentEvent{.type = AgentEventType::AssistantReady,
                              .text = text,
                              .entry_id = entry_id,
                              .thread_id = state->pending_thread_id,
                              .finish_reason = finish_reason,
                              .scoped_assist = state->turn_mode == AgentTurnMode::ScopedAssist,
                              .response_goal = state->turn_plan.response_goal,
                              .render_mode = state->turn_plan.render_mode});
}

void AgentSession::PushError(const std::shared_ptr<Impl>& state, const std::string& message) {
  PushEvent(state, AgentEvent{.type = AgentEventType::Error, .message = message});
}

void AgentSession::FinishTurn(const std::shared_ptr<Impl>& state) {
  state->turn_trace.Log();
  state->busy = false;
  state->iterations = 0;
  state->turn_scratch.clear();
  state->people_list_blocks.reset();
  state->pending_entry_id.clear();
  state->pending_thread_id.clear();
  state->turn_mode = AgentTurnMode::Conversation;
  PushLoading(state, false);
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
  const auto insert_at = state->turn_scratch.front().role == "system" ? state->turn_scratch.begin() + 1
                                                                      : state->turn_scratch.begin();
  state->turn_scratch.insert(insert_at, ChatMessage{.role = "system", .content = policy});
}

void AgentSession::PersistAssistantToThread(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                            std::string* out_message_id) {
  if (!state->thread_store || state->pending_thread_id.empty()) {
    return;
  }

  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = state->pending_thread_id;
  message.sender_contact_id = kAiAssistantContactId;
  message.text = assistant_raw;
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Local;
  message.relay_visible = state->turn_mode != AgentTurnMode::ScopedAssist;
  message.transport = MessageTransport::Local;
  (void)state->thread_store->AppendMessage(message);
  if (out_message_id) {
    *out_message_id = message.id;
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
    logging::getLogger("AgentSession").warning << "Assistant output parse failed: " << parsed.error;
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

  BrowserThread::PostTask(BrowserThreadId::IO, [state, tool_calls]() {
    if (state->cancelled) {
      AgentSession::FinishTurn(state);
      return;
    }

    for (const ToolCall& call : tool_calls) {
      AgentSession::PushToolActivity(state, call.name, "running");
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
      logging::getLogger("AgentSession").warning << "Synthesis returned embedded tool blocks; extracting";
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
    PushError(state, result.error().message);
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
    }
  }

  if (!state->llm) {
    return Error("LLM not configured");
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
    PushError(state, plan.error().message);
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

  TurnExecutionResult execution = TurnExecutor::Execute(state->turn_plan, state->tools, on_activity);
  if (!execution.ok) {
    PushError(state, execution.error);
    FinishTurn(state);
    return;
  }

  state->turn_trace.tools_executed = execution.tools_executed;
  state->turn_scratch.insert(state->turn_scratch.end(), execution.scratch_append.begin(), execution.scratch_append.end());
  state->people_list_blocks = execution.people_list_blocks;

  ContinueAfterExecution(state);
}

void AgentSession::StartTurn(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm) {
    FinishTurn(state);
    return;
  }

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

    ThreadMessage user_message;
    user_message.id = util::GenerateUuid();
    user_message.thread_id = state->pending_thread_id;
    user_message.sender_contact_id = kLocalSelfContactId;
    user_message.text = state->pending_user_text;
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
      PushError(state, messages.error().message);
      FinishTurn(state);
      return;
    }

    std::optional<ConversationSummary> summary;
    if (auto memory = state->thread_store->GetThreadMemory(state->pending_thread_id)) {
      summary = *memory;
    }

    ThreadContextPolicy policy(state->config.context);
    const std::string system_prompt = PromptBuilder::BuildChatAgentSystemPrompt(state->tools.SummaryForPrompt());
    const ContextBuildResult built = policy.Build(*messages, system_prompt, state->pending_user_text,
                                                  state->pending_user_payload, summary);
    state->turn_scratch = built.messages;
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

    auto messages = state->thread_store->GetMessagesForContext(state->pending_thread_id, state->config.context);
    if (!messages) {
      PushError(state, messages.error().message);
      FinishTurn(state);
      return;
    }

    std::optional<ConversationSummary> summary;
    if (auto memory = state->thread_store->GetThreadMemory(state->pending_thread_id)) {
      summary = *memory;
    }

    ThreadContextPolicy policy(state->config.context);
    state->turn_scratch = policy.BuildAssistContext(*messages, state->pending_user_text, summary);
    if (!state->turn_scratch.empty() && state->turn_scratch.front().role == "system") {
      state->turn_scratch.front().content =
          PromptBuilder::BuildScopedAssistSystemPrompt(state->tools.SummaryForPrompt());
    }
    RunTurnPipeline(state);
    return;
  }

  TranscriptEntry& entry = state->conversation.AppendUser(state->pending_user_text, state->pending_user_payload);
  state->pending_entry_id = entry.id;

  const std::string system_prompt = PromptBuilder::BuildChatAgentSystemPrompt(state->tools.SummaryForPrompt());
  const TurnSnapshot snapshot =
      state->coordinator.BeginTurn(state->conversation, system_prompt, entry, state->config.context);
  state->turn_scratch = snapshot.messages;
  RunTurnPipeline(state);
}

void AgentSession::RefreshCompactionService(const std::shared_ptr<Impl>& state) {
  if (state->thread_store && state->llm) {
    state->compaction = std::make_unique<ThreadCompactionService>(*state->thread_store, state->llm.get());
  } else {
    state->compaction.reset();
  }
}

void AgentSession::ConfigureOnIO(const std::shared_ptr<Impl>& state) {
  BrowserThread::PauseIO();

  state->llm = std::make_unique<LlmClient>(state->config.llm);

  const AppConfig defaults = SessionStore::Instance().DefaultConfig();
  state->mcp.Start(state->config, defaults);

  std::vector<std::string> custom_prefixes;
  custom_prefixes.reserve(state->config.mcp_servers.size());
  for (const McpConfig& entry : state->config.mcp_servers) {
    custom_prefixes.push_back(entry.id);
  }

  state->tools = ToolRegistry::BuildFromConfig(state->config, state->mcp.PromotedPtr(), state->mcp.CustomPtrs(),
                                               custom_prefixes);
  state->configured = true;
  RefreshCompactionService(state);

  logging::getLogger("AgentSession").info << "Agent configured with " << state->tools.Tools().size() << " tool(s)";
  for (const ToolDescriptor& tool : state->tools.Tools()) {
    logging::getLogger("AgentSession").info << "  - " << tool.definition.name;
  }

  if (MessagingHub::Instance().IsInitialized()) {
    const auto& bootstrap = SessionStore::Instance().Snapshot();
    (void)MessagingHub::Instance().Reinitialize(state->config, bootstrap.profile_data_dir);
  }

  BrowserThread::ResumeIO();

  if (state->submit_when_ready && !state->pending_user_text.empty()) {
    state->submit_when_ready = false;
    StartTurn(state);
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

  BrowserThread::PostTask(BrowserThreadId::IO, [impl = impl_]() { ConfigureOnIO(impl); });
}

McpClient* AgentSession::PromotedMcp() {
  return impl_->mcp.PromotedPtr();
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

void AgentSession::Submit(const std::string& user_text, std::optional<std::string> user_payload) {
  if (user_text.empty() || impl_->busy.exchange(true)) {
    return;
  }

  impl_->cancelled = false;
  impl_->pending_user_text = user_text;
  impl_->pending_user_payload = std::move(user_payload);
  impl_->turn_mode = AgentTurnMode::Conversation;
  impl_->pending_thread_id.clear();

  BrowserThread::PostTask(BrowserThreadId::IO, [impl = impl_]() {
    if (!impl->configured) {
      impl->submit_when_ready = true;
      return;
    }
    StartTurn(impl);
  });
}

void AgentSession::SubmitToThread(const std::string& thread_id, const std::string& user_text,
                                  std::optional<std::string> user_payload) {
  if (user_text.empty() || impl_->busy.exchange(true)) {
    return;
  }

  impl_->cancelled = false;
  impl_->pending_user_text = user_text;
  impl_->pending_user_payload = std::move(user_payload);
  impl_->pending_thread_id = thread_id;
  impl_->turn_mode = AgentTurnMode::Thread;

  BrowserThread::PostTask(BrowserThreadId::IO, [impl = impl_]() {
    if (!impl->configured) {
      impl->submit_when_ready = true;
      return;
    }
    StartTurn(impl);
  });
}

void AgentSession::SubmitScopedAssist(const std::string& thread_id, const std::string& prompt,
                                      std::optional<std::string> user_payload) {
  if (prompt.empty() || impl_->busy.exchange(true)) {
    return;
  }

  impl_->cancelled = false;
  impl_->pending_user_text = prompt;
  impl_->pending_user_payload = std::move(user_payload);
  impl_->pending_thread_id = thread_id;
  impl_->turn_mode = AgentTurnMode::ScopedAssist;

  BrowserThread::PostTask(BrowserThreadId::IO, [impl = impl_]() {
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
  impl_->busy = false;
}

void AgentSession::StartNewConversation() {
  BrowserThread::PostTask(BrowserThreadId::IO, [impl = impl_]() {
    impl->conversation.StartNewConversation();
    impl->turn_scratch.clear();
    impl->pending_entry_id.clear();
    impl->pending_user_text.clear();
    impl->pending_user_payload.reset();
    impl->cancelled = false;
    impl->busy = false;
  });
}

} // namespace pbr
