#include "agent/AgentSession.h"

#include "agent/LlmClient.h"
#include "agent/PromptBuilder.h"
#include "agent/ToolRegistry.h"
#include "mcp/McpClient.h"
#include "platform/BrowserThread.h"

#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pbr {

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
  std::vector<ChatMessage> history;
  std::string pending_user_text;
  int iterations = 0;
  bool configured = false;
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

void AgentSession::PushAssistantReady(const std::shared_ptr<Impl>& state, const std::string& text) {
  PushEvent(state, AgentEvent{.type = AgentEventType::AssistantReady, .text = text});
}

void AgentSession::PushError(const std::shared_ptr<Impl>& state, const std::string& message) {
  PushEvent(state, AgentEvent{.type = AgentEventType::Error, .message = message});
}

void AgentSession::FinishTurn(const std::shared_ptr<Impl>& state) {
  state->busy = false;
  state->iterations = 0;
  PushLoading(state, false);
}

void AgentSession::HandleLlmResponse(const std::shared_ptr<Impl>& state, const ChatCompletionResponse& response) {
  if (state->cancelled) {
    FinishTurn(state);
    return;
  }

  if (!response.tool_calls.empty()) {
    ChatMessage assistant_message;
    assistant_message.role = "assistant";
    assistant_message.content = response.content.value_or("");
    assistant_message.tool_calls = ToolCallsToJson(response.tool_calls);
    state->history.push_back(std::move(assistant_message));

    BrowserThread::PostTask(BrowserThreadId::IO, [state, tool_calls = response.tool_calls]() {
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
          tool_message.content = *result;
        } else {
          tool_message.content = "Tool error: " + result.error().message;
        }
        state->history.push_back(std::move(tool_message));
      }

      ++state->iterations;
      if (state->iterations >= kMaxIterations) {
        AgentSession::PushError(state, "Agent iteration limit reached");
        AgentSession::FinishTurn(state);
        return;
      }

      AgentSession::RunLlmStep(state);
    });
    return;
  }

  if (!response.content || response.content->empty()) {
    PushError(state, "LLM response missing content");
    FinishTurn(state);
    return;
  }

  PushAssistantReady(state, *response.content);
  FinishTurn(state);
}

void AgentSession::RunLlmStep(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm) {
    FinishTurn(state);
    return;
  }

  ChatCompletionRequest request;
  request.messages = state->history;
  request.tools = state->tools.Definitions();

  auto result = state->llm->Complete(request);
  if (!result) {
    PushError(state, result.error().message);
    FinishTurn(state);
    return;
  }

  HandleLlmResponse(state, *result);
}

void AgentSession::StartTurn(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm) {
    FinishTurn(state);
    return;
  }

  state->history.clear();
  state->iterations = 0;

  const std::string system_prompt =
      PromptBuilder::BuildChatAgentSystemPrompt(state->tools.SummaryForPrompt());
  state->history.push_back(ChatMessage{.role = "system", .content = system_prompt});
  state->history.push_back(ChatMessage{.role = "user", .content = state->pending_user_text});

  PushLoading(state, true);
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
}

AgentSession::AgentSession() : impl_(std::make_shared<Impl>()) {}

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

void AgentSession::Submit(const std::string& user_text) {
  if (user_text.empty() || impl_->busy.exchange(true)) {
    return;
  }

  impl_->cancelled = false;
  impl_->pending_user_text = user_text;

  BrowserThread::PostTask(BrowserThreadId::IO, [impl = impl_]() { StartTurn(impl); });
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

} // namespace pbr
