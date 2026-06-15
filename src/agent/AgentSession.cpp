#include "agent/AgentSession.h"

#include "agent/LlmClient.h"
#include "agent/PromptBuilder.h"
#include "agent/SearchIntent.h"
#include "agent/StructuredTextParser.h"
#include "agent/ToolRegistry.h"
#include "agent/conversation/Conversation.h"
#include "agent/conversation/TurnCoordinator.h"
#include "log/Logger.h"
#include "mcp/McpClient.h"
#include "platform/BrowserThread.h"

#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

constexpr int kMaxIterations = 8;

std::string FormatToolResultForLlm(const std::string& tool_name, const std::string& raw_result) {
  if (tool_name == "web_search") {
    return PromptBuilder::FormatSearchResultsForLlm(raw_result);
  }
  return raw_result;
}

nlohmann::json ToolCallsToJson(const std::vector<ToolCall>& tool_calls) {
  nlohmann::json out = nlohmann::json::array();
  for (const ToolCall& call : tool_calls) {
    out.push_back({{"id", call.id},
                   {"type", "function"},
                   {"function", {{"name", call.name}, {"arguments", call.arguments.dump()}}}});
  }
  return out;
}

} // namespace

struct AgentSession::Impl {
  std::mutex event_mutex;
  std::vector<AgentEvent> pending_events;

  std::atomic<bool> cancelled{false};
  std::atomic<bool> busy{false};

  AppConfig config;
  std::unique_ptr<LlmClient> llm;
  std::unique_ptr<McpClient> mcp;
  ToolRegistry tools;
  Conversation conversation;
  TurnCoordinator coordinator;
  std::vector<ChatMessage> turn_scratch;
  std::string pending_user_text;
  std::optional<std::string> pending_user_payload;
  std::string pending_entry_id;
  int iterations = 0;
  bool configured = false;
  bool submit_when_ready = false;
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
                            .finish_reason = finish_reason});
}

void AgentSession::PushError(const std::shared_ptr<Impl>& state, const std::string& message) {
  PushEvent(state, AgentEvent{.type = AgentEventType::Error, .message = message});
}

void AgentSession::FinishTurn(const std::shared_ptr<Impl>& state) {
  state->busy = false;
  state->iterations = 0;
  state->turn_scratch.clear();
  state->pending_entry_id.clear();
  PushLoading(state, false);
}

void AgentSession::DispatchToolCalls(const std::shared_ptr<Impl>& state, const std::vector<ToolCall>& tool_calls,
                                     const std::string& assistant_content) {
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

    ++state->iterations;
    if (state->iterations >= kMaxIterations) {
      AgentSession::PushError(state, "Agent iteration limit reached");
      AgentSession::FinishTurn(state);
      return;
    }

    AgentSession::RunLlmStep(state);
  });
}

void AgentSession::HandleLlmResponse(const std::shared_ptr<Impl>& state, const ChatCompletionResponse& response) {
  if (state->cancelled) {
    FinishTurn(state);
    return;
  }

  if (!response.tool_calls.empty()) {
    DispatchToolCalls(state, response.tool_calls, response.content.value_or(""));
    return;
  }

  if (response.content) {
    if (auto embedded = StructuredTextParser::ExtractEmbeddedToolCalls(*response.content)) {
      std::vector<ToolCall> tool_calls;
      tool_calls.reserve(embedded->size());
      for (size_t i = 0; i < embedded->size(); ++i) {
        tool_calls.push_back(ToolCall{
            .id = "embedded_" + std::to_string(i + 1),
            .name = (*embedded)[i].name,
            .arguments = (*embedded)[i].arguments,
        });
      }
      DispatchToolCalls(state, tool_calls, *response.content);
      return;
    }
  }

  if (!response.content || response.content->empty()) {
    PushError(state, "LLM response missing content");
    FinishTurn(state);
    return;
  }

  state->coordinator.CompleteTurn(state->conversation, state->pending_entry_id, *response.content);
  PushAssistantReady(state, state->pending_entry_id, *response.content, response.finish_reason);
  FinishTurn(state);
}

void AgentSession::RunLlmStep(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm) {
    FinishTurn(state);
    return;
  }

  ChatCompletionRequest request;
  request.messages = state->turn_scratch;
  request.tools = state->tools.Definitions();

  auto result = state->llm->Complete(request);
  if (!result) {
    PushError(state, result.error().message);
    FinishTurn(state);
    return;
  }

  HandleLlmResponse(state, *result);
}

void AgentSession::RunProactiveSearchAndLlm(const std::shared_ptr<Impl>& state) {
  PushToolActivity(state, "web_search", "running");

  const std::string search_query = BuildWebSearchQuery(state->pending_user_text);
  nlohmann::json args = {{"query", search_query}};
  auto search_result = state->tools.Execute("web_search", args);

  if (search_result) {
    PushToolActivity(state, "web_search", "done");
    state->turn_scratch[0].content +=
        "\n\n" + PromptBuilder::BuildProactiveSearchContext(search_query, *search_result);

    ChatMessage assistant_message;
    assistant_message.role = "assistant";
    assistant_message.content = "";
    assistant_message.tool_calls = ToolCallsToJson({ToolCall{
        .id = "proactive_1",
        .name = "web_search",
        .arguments = args,
    }});
    state->turn_scratch.push_back(std::move(assistant_message));

    ChatMessage tool_message;
    tool_message.role = "tool";
    tool_message.tool_call_id = "proactive_1";
    tool_message.content = FormatToolResultForLlm("web_search", *search_result);
    state->turn_scratch.push_back(std::move(tool_message));

    logging::getLogger("AgentSession").info << "Proactive web_search completed for: " << state->pending_user_text;
  } else {
    PushToolActivity(state, "web_search", "error");
    logging::getLogger("AgentSession").warning
        << "Proactive web_search failed: " << search_result.error().message;
  }

  ++state->iterations;
  RunLlmStep(state);
}

void AgentSession::StartTurn(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm) {
    FinishTurn(state);
    return;
  }

  state->iterations = 0;
  state->turn_scratch.clear();

  TranscriptEntry& entry = state->conversation.AppendUser(state->pending_user_text, state->pending_user_payload);
  state->pending_entry_id = entry.id;

  const std::string system_prompt =
      PromptBuilder::BuildChatAgentSystemPrompt(state->tools.SummaryForPrompt());
  const TurnSnapshot snapshot =
      state->coordinator.BeginTurn(state->conversation, system_prompt, entry, state->config.context);
  state->turn_scratch = snapshot.messages;

  PushLoading(state, true);

  if (ShouldProactiveWebSearch(state->pending_user_text)) {
    logging::getLogger("AgentSession").info << "Proactive web_search triggered for: " << state->pending_user_text;
    RunProactiveSearchAndLlm(state);
    return;
  }

  RunLlmStep(state);
}

void AgentSession::ConfigureOnIO(const std::shared_ptr<Impl>& state) {
  state->llm = std::make_unique<LlmClient>(state->config.llm);

  state->mcp = std::make_unique<McpClient>();
  if (state->config.mcp && !state->config.mcp->command.empty()) {
    if (state->config.mcp->command == "mock") {
      state->mcp->Start("mock");
    } else {
      state->mcp->Start(state->config.mcp->command, state->config.mcp->args);
    }
    state->mcp->Initialize();
  }

  state->tools = ToolRegistry::BuildFromConfig(state->config, state->mcp.get());
  state->configured = true;

  logging::getLogger("AgentSession").info << "Agent configured with " << state->tools.Tools().size() << " tool(s)";
  for (const ToolDescriptor& tool : state->tools.Tools()) {
    logging::getLogger("AgentSession").info << "  - " << tool.definition.name;
  }

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
                                       std::vector<TranscriptSuggestion> suggestions) {
  return impl_->conversation.SetAssistantDisplay(entry_id, assistant_rml, std::move(suggestions));
}

void AgentSession::Submit(const std::string& user_text, std::optional<std::string> user_payload) {
  if (user_text.empty() || impl_->busy.exchange(true)) {
    return;
  }

  impl_->cancelled = false;
  impl_->pending_user_text = user_text;
  impl_->pending_user_payload = std::move(user_payload);

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
