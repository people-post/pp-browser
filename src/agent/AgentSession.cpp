#include "agent/AgentSession.h"

#include "agent/LlmClient.h"
#include "agent/PromptBuilder.h"
#include "agent/SearchIntent.h"
#include "agent/StructuredTextParser.h"
#include "agent/ToolRegistry.h"
#include "agent/conversation/Conversation.h"
#include "agent/conversation/ThreadContextPolicy.h"
#include "agent/conversation/TurnCoordinator.h"
#include "log/Logger.h"
#include "mcp/McpClient.h"
#include "messaging/IdUtil.h"
#include "messaging/IThreadStore.h"
#include "messaging/MessagingJson.h"
#include "messaging/PeopleDiscoveryBlocks.h"
#include "messaging/ThreadTypes.h"
#include "platform/BrowserThread.h"

#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pbr {

enum class AgentTurnMode { Conversation, Thread, ScopedAssist };

namespace {

constexpr int kMaxIterations = 8;

std::string FormatToolResultForLlm(const std::string& tool_name, const std::string& raw_result) {
  if (tool_name == "web_search") {
    return PromptBuilder::FormatSearchResultsForLlm(raw_result);
  }
  if (tool_name == "search_people" || tool_name == "list_contacts") {
    if (const std::string blocks = TryPeopleDiscoveryBlocksFromToolJson(raw_result); !blocks.empty()) {
      return "People discovery results (render with long_list; do not expose this JSON verbatim):\n" + blocks;
    }
  }
  return raw_result;
}

bool IsPeopleDiscoveryTool(const std::string& name) {
  return name == "search_people" || name == "list_contacts";
}

void ParsePeopleToolJson(const std::string& raw, std::vector<DirectoryHit>& hits, std::vector<Contact>& contacts) {
  const nlohmann::json doc = nlohmann::json::parse(raw, nullptr, false);
  if (doc.is_discarded() || !doc.is_array()) {
    return;
  }
  for (const auto& item : doc) {
    if (!item.is_object()) {
      continue;
    }
    if (item.contains("hit_id")) {
      hits.push_back(DirectoryHitFromJson(item));
    } else if (item.contains("id") && item.contains("display_name")) {
      contacts.push_back(ContactFromJson(item));
    }
  }
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

  IThreadStore* thread_store = nullptr;
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
                              .scoped_assist = state->turn_mode == AgentTurnMode::ScopedAssist});
}

void AgentSession::PushError(const std::shared_ptr<Impl>& state, const std::string& message) {
  PushEvent(state, AgentEvent{.type = AgentEventType::Error, .message = message});
}

void AgentSession::FinishTurn(const std::shared_ptr<Impl>& state) {
  state->busy = false;
  state->iterations = 0;
  state->turn_scratch.clear();
  state->pending_entry_id.clear();
  state->pending_thread_id.clear();
  state->turn_mode = AgentTurnMode::Conversation;
  PushLoading(state, false);
}

bool AgentSession::TryFinishPeopleDiscoveryTurn(const std::shared_ptr<Impl>& state,
                                                const std::vector<ToolCall>& tool_calls,
                                                const std::vector<Roe<std::string>>& tool_results) {
  if (state->turn_mode == AgentTurnMode::ScopedAssist || tool_calls.empty() ||
      tool_results.size() != tool_calls.size()) {
    return false;
  }

  std::vector<DirectoryHit> hits;
  std::vector<Contact> contacts;
  for (size_t i = 0; i < tool_calls.size(); ++i) {
    if (!IsPeopleDiscoveryTool(tool_calls[i].name)) {
      return false;
    }
    if (!tool_results[i]) {
      return false;
    }
    ParsePeopleToolJson(*tool_results[i], hits, contacts);
  }

  const std::string blocks_json = BuildPeopleDiscoveryBlocksJson(hits, contacts);
  std::string assistant_message_id;
  PersistAssistantToThread(state, blocks_json, &assistant_message_id);
  PushAssistantReady(state, assistant_message_id, blocks_json, "stop");
  FinishTurn(state);
  return true;
}

bool AgentSession::FinishPeopleDiscoveryFromIntent(const std::shared_ptr<Impl>& state) {
  const bool contacts_list = ShouldProactiveContactsList(state->pending_user_text);
  const std::string tool_name = contacts_list ? "list_contacts" : "search_people";
  const std::string query = BuildPeopleSearchQuery(state->pending_user_text);
  const nlohmann::json args = {{"query", query}};

  PushToolActivity(state, tool_name, "running");
  auto result = state->tools.Execute(tool_name, args);
  PushToolActivity(state, tool_name, result ? "done" : "error");
  if (!result) {
    PushError(state, result.error().message);
    FinishTurn(state);
    return false;
  }

  std::vector<DirectoryHit> hits;
  std::vector<Contact> contacts;
  ParsePeopleToolJson(*result, hits, contacts);

  const std::string blocks_json = BuildPeopleDiscoveryBlocksJson(hits, contacts);
  std::string assistant_message_id;
  PersistAssistantToThread(state, blocks_json, &assistant_message_id);
  PushAssistantReady(state, assistant_message_id, blocks_json, "stop");
  FinishTurn(state);
  logging::getLogger("AgentSession").info << "People discovery completed for: " << state->pending_user_text;
  return true;
}

void AgentSession::RunProactivePeopleDiscovery(const std::shared_ptr<Impl>& state) {
  (void)FinishPeopleDiscoveryFromIntent(state);
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

    std::vector<Roe<std::string>> tool_results;
    tool_results.reserve(tool_calls.size());

    for (const ToolCall& call : tool_calls) {
      AgentSession::PushToolActivity(state, call.name, "running");
      auto result = state->tools.Execute(call.name, call.arguments);
      AgentSession::PushToolActivity(state, call.name, result ? "done" : "error");
      tool_results.push_back(result);

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

    if (AgentSession::TryFinishPeopleDiscoveryTurn(state, tool_calls, tool_results)) {
      return;
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

  if (state->turn_mode != AgentTurnMode::Conversation) {
    const ParseResult parsed = StructuredTextParser::ParseFromLlmOutput(*response.content);
    if (!parsed.ok && ShouldProactivePeopleDiscovery(state->pending_user_text)) {
      if (FinishPeopleDiscoveryFromIntent(state)) {
        return;
      }
    }

    std::string assistant_message_id;
    PersistAssistantToThread(state, *response.content, &assistant_message_id);
    PushAssistantReady(state, assistant_message_id, *response.content, response.finish_reason);
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

void AgentSession::PersistAssistantToThread(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                            std::string* out_message_id) {
  if (!state->thread_store || state->pending_thread_id.empty()) {
    return;
  }

  ThreadMessage message;
  message.id = GenerateUuid();
  message.thread_id = state->pending_thread_id;
  message.sender_contact_id = kAiAssistantContactId;
  message.text = assistant_raw;
  message.timestamp = NowUnixMs();
  message.delivery = MessageDelivery::Local;
  message.relay_visible = state->turn_mode != AgentTurnMode::ScopedAssist;
  (void)state->thread_store->AppendMessage(message);
  if (out_message_id) {
    *out_message_id = message.id;
  }
}

void AgentSession::StartThreadTurn(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm || !state->thread_store) {
    FinishTurn(state);
    return;
  }

  state->iterations = 0;
  state->turn_scratch.clear();

  ThreadMessage user_message;
  user_message.id = GenerateUuid();
  user_message.thread_id = state->pending_thread_id;
  user_message.sender_contact_id = kLocalSelfContactId;
  user_message.text = state->pending_user_text;
  user_message.timestamp = NowUnixMs();
  user_message.delivery = MessageDelivery::Local;
  if (auto appended = state->thread_store->AppendMessage(user_message)) {
    state->pending_entry_id = appended->id;
  } else {
    state->pending_entry_id = user_message.id;
  }

  auto messages = state->thread_store->GetMessages(state->pending_thread_id);
  if (!messages) {
    PushError(state, messages.error().message);
    FinishTurn(state);
    return;
  }

  ThreadContextPolicy policy(state->config.context);
  const std::string system_prompt = PromptBuilder::BuildChatAgentSystemPrompt(state->tools.SummaryForPrompt());
  const ContextBuildResult built =
      policy.Build(*messages, system_prompt, state->pending_user_text, state->pending_user_payload);
  state->turn_scratch = built.messages;

  PushLoading(state, true);

  if (ShouldProactivePeopleDiscovery(state->pending_user_text)) {
    RunProactivePeopleDiscovery(state);
    return;
  }

  if (ShouldProactiveWebSearch(state->pending_user_text)) {
    RunProactiveSearchAndLlm(state);
    return;
  }

  RunLlmStep(state);
}

void AgentSession::StartScopedAssistTurn(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm || !state->thread_store) {
    FinishTurn(state);
    return;
  }

  state->iterations = 0;
  state->turn_scratch.clear();
  state->pending_entry_id = GenerateUuid();

  auto messages = state->thread_store->GetMessages(state->pending_thread_id);
  if (!messages) {
    PushError(state, messages.error().message);
    FinishTurn(state);
    return;
  }

  ThreadContextPolicy policy(state->config.context);
  state->turn_scratch = policy.BuildAssistContext(*messages, state->pending_user_text);

  PushLoading(state, true);
  RunLlmStep(state);
}

void AgentSession::StartTurn(const std::shared_ptr<Impl>& state) {
  if (state->turn_mode == AgentTurnMode::Thread) {
    StartThreadTurn(state);
    return;
  }
  if (state->turn_mode == AgentTurnMode::ScopedAssist) {
    StartScopedAssistTurn(state);
    return;
  }

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
  if (state->config.mcp && state->config.mcp->IsConfigured()) {
    if (!state->config.mcp->url.empty()) {
      state->mcp->StartHttp(state->config.mcp->url);
    } else if (state->config.mcp->command == "mock") {
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
                                       std::vector<TranscriptChatAction> chat_actions) {
  return impl_->conversation.SetAssistantDisplay(entry_id, assistant_rml, std::move(chat_actions));
}

void AgentSession::SetThreadStore(IThreadStore* store) {
  impl_->thread_store = store;
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
