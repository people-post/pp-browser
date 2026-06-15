#include "demo/ChatDemo.h"

#include "agent/StructuredTextParser.h"
#include "agent/conversation/Conversation.h"
#include "app/Application.h"
#include "app/InputCoordinator.h"
#include "ui/DataModelHost.h"
#include "ui/DocumentLoader.h"
#include "ui/SplitLayoutHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {

namespace {

std::string ToolActivityLabel(const std::string& tool_name, const std::string& status) {
  if (tool_name == "web_search") {
    if (status == "running") {
      return "Searching the web...";
    }
    if (status == "done") {
      return "Search complete";
    }
    if (status == "error") {
      return "Search failed";
    }
  }
  if (status == "running") {
    return "Running " + tool_name + "...";
  }
  if (status == "done") {
    return "Finished " + tool_name;
  }
  return tool_name;
}

std::string Trim(const std::string& text) {
  const auto start = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return {};
  }
  return std::string(start, end);
}

std::string ToLower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

Rml::String UserMessageRml(const std::string& text) {
  return Rml::String(("<div class=\"bubble bubble-user\" selectable=\"text\"><p>" + StructuredTextParser::EscapeText(text) +
                      "</p></div>")
                         .c_str());
}

Rml::String AssistantBubbleRml(const std::string& rml) {
  return Rml::String(rml.c_str());
}

std::string InlineSuggestionButtonsRml(const std::vector<TranscriptSuggestion>& suggestions) {
  std::ostringstream out;
  for (const TranscriptSuggestion& suggestion : suggestions) {
    out << "<button class=\"chat-suggestion\" data-event-click=\"send_suggestion('"
        << StructuredTextParser::EscapeExpressionString(suggestion.message) << "')\">"
        << StructuredTextParser::EscapeText(suggestion.label) << "</button>";
  }
  return out.str();
}

std::string HydrateLegacySuggestions(const std::string& assistant_rml, const std::vector<TranscriptSuggestion>& suggestions) {
  if (suggestions.empty() || assistant_rml.find("chat-suggestion") != std::string::npos) {
    return assistant_rml;
  }

  const std::string buttons = InlineSuggestionButtonsRml(suggestions);
  constexpr const char* stack_close = "</div>";
  if (assistant_rml.size() > 6 && assistant_rml.find("<div class=\"stack\">") == 0 &&
      assistant_rml.compare(assistant_rml.size() - 6, 6, stack_close) == 0) {
    return assistant_rml.substr(0, assistant_rml.size() - 6) + buttons + stack_close;
  }
  return assistant_rml + buttons;
}

Rml::String ErrorMessageRml(const std::string& message) {
  return Rml::String(("<p class=\"error\">" + StructuredTextParser::EscapeText(message) + "</p>").c_str());
}

std::string TruncatePreview(const std::string& text, size_t max_len = 48) {
  if (text.size() <= max_len) {
    return text;
  }
  return text.substr(0, max_len - 3) + "...";
}

std::string MockAssistantRespond(const std::string& query) {
  const std::string lower = ToLower(query);

  if (lower.find("help") != std::string::npos) {
    return R"({
      "blocks": [
        { "type": "heading", "level": 2, "text": "Help" },
        { "type": "paragraph", "text": "pp-browser renders structured JSON blocks, not HTML or markdown." },
        { "type": "list", "ordered": false, "items": [
          "Type a message and click Send",
          "Try help, list, code, or button for sample replies",
          "Press Escape to quit"
        ]}
      ]
    })";
  }

  if (lower.find("list") != std::string::npos) {
    return R"({
      "blocks": [
        { "type": "paragraph", "text": "Here is an ordered list:" },
        { "type": "list", "ordered": true, "items": ["First item", "Second item", "Third item"] }
      ]
    })";
  }

  if (lower.find("code") != std::string::npos) {
    return R"({
      "blocks": [
        { "type": "paragraph", "text": "Example code block:" },
        { "type": "code", "text": "auto result = StructuredTextParser::ParseBlocksJson(json);\nif (result.ok) { /* render */ }" }
      ]
    })";
  }

  if (lower.find("button") != std::string::npos) {
    return R"({
      "blocks": [
        { "type": "paragraph", "text": "Click a suggestion to send it as your next message:" },
        { "type": "button", "label": "Explain more", "message": "Can you explain that in simpler terms?" },
        { "type": "button", "label": "Give an example", "message": "Can you give a concrete example?" }
      ]
    })";
  }

  return R"({
    "blocks": [
      { "type": "paragraph", "text": "Thanks for your message. This is a mock assistant response." },
      { "type": "paragraph", "text": "Try typing help, list, code, or button to see other structured reply formats." }
    ]
  })";
}

void DirtyChat() {
  DataModelHost::Instance().Dirty("chat", "draft");
  DataModelHost::Instance().Dirty("chat", "status");
  DataModelHost::Instance().Dirty("chat", "turns");
  DataModelHost::Instance().Dirty("chat", "loading");
}

void DirtyShell() {
  DataModelHost::Instance().Dirty("shell", "sessions");
  DataModelHost::Instance().Dirty("shell", "preview_rml");
}

} // namespace

ChatDemo::ChatDemo() {
  redirectLogger("ChatDemo");
}

ChatDemo& ChatDemo::Instance() {
  static ChatDemo demo;
  return demo;
}

void ChatDemo::SendMessageCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                   const Rml::VariantList& /*args*/) {
  Instance().OnSendMessage();
}

void ChatDemo::SendSuggestionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SendUserText(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatDemo::NewChatCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& /*args*/) {
  Instance().OnNewChat();
}

void ChatDemo::OnSendMessage() {
  if (chat_.loading) {
    return;
  }

  const std::string text = Trim(chat_.draft.c_str());
  if (text.empty()) {
    return;
  }

  chat_.draft = "";
  DirtyChat();
  SendUserText(text);
}

void ChatDemo::OnNewChat() {
  if (agent_) {
    agent_->StartNewConversation();
  }
  chat_.draft = "";
  chat_.status = "";
  chat_.loading = false;
  chat_.turns.clear();
  shell_.preview_rml = "";
  shell_.sessions = {{Rml::String("Chat"), Rml::String("Ask anything...")}};
  pending_reply_.reset();
  DirtyChat();
  DirtyShell();
}

void ChatDemo::SyncDisplayFromConversation() {
  if (!agent_) {
    return;
  }

  chat_.turns.clear();
  for (const TranscriptEntry& entry : agent_->conversation().Entries()) {
    TranscriptDisplayRow row;
    row.user_content_rml = UserMessageRml(entry.user_text);
    if (entry.assistant_rml) {
      row.assistant_content_rml =
          AssistantBubbleRml(HydrateLegacySuggestions(*entry.assistant_rml, entry.suggestions));
      row.has_assistant = true;
    } else if (entry.assistant_raw && !entry.assistant_rml) {
      row.assistant_content_rml = ErrorMessageRml("Assistant reply pending display sync.");
      row.has_assistant = true;
    }
    chat_.turns.push_back(std::move(row));
  }
  DirtyChat();
}

void ChatDemo::UpdateSidebarPreview(const std::string& preview_text) {
  if (shell_.sessions.empty()) {
    shell_.sessions.push_back({Rml::String("Chat"), Rml::String("Ask anything...")});
  }
  shell_.sessions[0].preview = Rml::String(TruncatePreview(preview_text).c_str());
  DirtyShell();
}

void ChatDemo::SendUserText(const std::string& text) {
  const std::string trimmed = Trim(text);
  if (trimmed.empty() || chat_.loading) {
    return;
  }

  chat_.loading = true;
  chat_.status = "";
  UpdateSidebarPreview(trimmed);
  DirtyChat();

  if (!use_llm_) {
    if (!agent_) {
      return;
    }
    TranscriptEntry& entry = agent_->AppendUserMessage(trimmed);
    SyncDisplayFromConversation();
    log().debug << "Using mock assistant response";
    pending_reply_ = PendingReply{.entry_id = entry.id, .output = MockAssistantRespond(trimmed), .from_llm = false};
    return;
  }

  log().info << "Submitting message to agent session";
  agent_->Submit(trimmed);
}

void ChatDemo::FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, bool from_llm,
                                    const std::string& finish_reason) {
  auto parsed = from_llm ? StructuredTextParser::ParseFromLlmOutput(raw_output)
                         : StructuredTextParser::ParseBlocksJson(raw_output);
  if (!parsed.ok) {
    log().warning << "Failed to parse assistant reply: " << parsed.error;
    if (from_llm && !finish_reason.empty()) {
      log().warning << "LLM finish_reason: " << finish_reason;
    }
    log().warning << "AI response: " << raw_output;
    if (agent_) {
      if (!from_llm) {
        agent_->CompleteAssistantMessage(entry_id, raw_output);
      }
      agent_->SetAssistantDisplay(entry_id, parsed.error, {});
    }
  } else {
    std::vector<TranscriptSuggestion> suggestions;
    suggestions.reserve(parsed.suggestions.size());
    for (const ParsedSuggestion& suggestion : parsed.suggestions) {
      suggestions.push_back({suggestion.label, suggestion.message});
    }
    if (agent_) {
      if (!from_llm) {
        agent_->CompleteAssistantMessage(entry_id, raw_output);
      }
      agent_->SetAssistantDisplay(entry_id, parsed.rml, std::move(suggestions));
    }
    shell_.preview_rml = Rml::String(parsed.rml.c_str());
    DirtyShell();
  }

  SyncDisplayFromConversation();
  chat_.loading = false;
  chat_.status = "";
  DirtyChat();
}

void ChatDemo::HandleAgentEvent(const AgentEvent& event) {
  switch (event.type) {
  case AgentEventType::LoadingChanged:
    chat_.loading = event.loading;
    if (event.loading) {
      SyncDisplayFromConversation();
    }
    if (!event.loading) {
      chat_.status = "";
    }
    DirtyChat();
    break;
  case AgentEventType::ToolActivity:
    chat_.status = Rml::String(ToolActivityLabel(event.tool_name, event.status).c_str());
    DirtyChat();
    break;
  case AgentEventType::AssistantReady:
    FinishAssistantReply(event.entry_id, event.text, true, event.finish_reason);
    break;
  case AgentEventType::Error:
    log().error << "Agent session error: " << event.message;
    if (agent_ && !agent_->conversation().Entries().empty()) {
      const TranscriptEntry& entry = agent_->conversation().Entries().back();
      agent_->CompleteAssistantMessage(entry.id, event.message);
      agent_->SetAssistantDisplay(entry.id, event.message, {});
      SyncDisplayFromConversation();
    }
    chat_.loading = false;
    chat_.status = "";
    DirtyChat();
    break;
  }
}

bool ChatDemo::Setup(Rml::Context* context, const AppConfig& config) {
  if (!context) {
    return false;
  }

  chat_ = {};
  shell_ = {};
  shell_.sessions = {{Rml::String("Chat"), Rml::String("Ask anything...")}};
  pending_reply_.reset();
  use_llm_ = !config.llm.base_url.empty();
  agent_.emplace();
  agent_->Configure(config);
  log().info << "Chat demo initialized (model: " << config.llm.model << ")";

  DataModelHost::Instance().Clear();

  const auto register_enter_send = [this](Rml::Input::KeyIdentifier key) {
    InputCoordinator::Instance().Register(KeyBinding{
        .key = key,
        .forbidden_modifiers = Rml::Input::KM_SHIFT,
        .when = [](Rml::Context* ctx) {
          Rml::Element* focus = ctx ? ctx->GetFocusElement() : nullptr;
          return focus && focus->GetId() == "draft-input";
        },
        .action = [this]() {
          OnSendMessage();
          return false;
        },
        .priority = 50,
    });
  };
  register_enter_send(Rml::Input::KI_RETURN);
  register_enter_send(Rml::Input::KI_NUMPADENTER);

  if (!DataModelHost::Instance().Register(context, "chat", [](Rml::DataModelConstructor& ctor) {
        if (auto sug_handle = ctor.RegisterStruct<ChatSuggestion>()) {
          sug_handle.RegisterMember("label", &ChatSuggestion::label);
          sug_handle.RegisterMember("message", &ChatSuggestion::message);
        }
        ctor.RegisterArray<std::vector<ChatSuggestion>>();
        if (auto turn_handle = ctor.RegisterStruct<TranscriptDisplayRow>()) {
          turn_handle.RegisterMember("user_content_rml", &TranscriptDisplayRow::user_content_rml);
          turn_handle.RegisterMember("assistant_content_rml", &TranscriptDisplayRow::assistant_content_rml);
          turn_handle.RegisterMember("has_assistant", &TranscriptDisplayRow::has_assistant);
          turn_handle.RegisterMember("suggestions", &TranscriptDisplayRow::suggestions);
        }
        ctor.RegisterArray<std::vector<TranscriptDisplayRow>>();
        ctor.Bind("draft", &ChatDemo::Instance().chat_.draft);
        ctor.Bind("status", &ChatDemo::Instance().chat_.status);
        ctor.Bind("loading", &ChatDemo::Instance().chat_.loading);
        ctor.Bind("turns", &ChatDemo::Instance().chat_.turns);
        ctor.BindEventCallback("send_message", &ChatDemo::SendMessageCallback);
        ctor.BindEventCallback("send_suggestion", &ChatDemo::SendSuggestionCallback);
      })) {
    return false;
  }

  if (!DataModelHost::Instance().Register(context, "shell", [](Rml::DataModelConstructor& ctor) {
        if (auto session_handle = ctor.RegisterStruct<ChatDemo::SessionRow>()) {
          session_handle.RegisterMember("title", &ChatDemo::SessionRow::title);
          session_handle.RegisterMember("preview", &ChatDemo::SessionRow::preview);
        }
        ctor.RegisterArray<std::vector<ChatDemo::SessionRow>>();
        ctor.Bind("sessions", &ChatDemo::Instance().shell_.sessions);
        ctor.Bind("preview_rml", &ChatDemo::Instance().shell_.preview_rml);
        ctor.BindEventCallback("new_chat", &ChatDemo::NewChatCallback);
        ctor.BindEventCallback("split_panel_h", &SplitLayoutHost::SplitPanelHCallback);
        ctor.BindEventCallback("split_panel_v", &SplitLayoutHost::SplitPanelVCallback);
        ctor.BindEventCallback("close_panel", &SplitLayoutHost::ClosePanelCallback);
        ctor.BindEventCallback("gutter_drag_start", &SplitLayoutHost::GutterDragStartCallback);
        ctor.BindEventCallback("gutter_drag_end", &SplitLayoutHost::GutterDragEndCallback);
      })) {
    return false;
  }

  SplitLayoutHost::Instance().Initialize(context);

  if (DocumentLoader::LoadFile(context, Application::AssetsPath("samples/chat_shell.rml")) == nullptr) {
    return false;
  }

  SplitLayoutHost::Instance().SyncLayout();
  return true;
}

void ChatDemo::Update() {
  if (pending_reply_) {
    PendingReply reply = std::move(*pending_reply_);
    pending_reply_.reset();
    FinishAssistantReply(reply.entry_id, reply.output, reply.from_llm);
  }

  if (!agent_) {
    return;
  }

  std::vector<AgentEvent> events;
  agent_->PollEvents(events);
  for (const AgentEvent& event : events) {
    HandleAgentEvent(event);
  }
}

void ChatDemo::Shutdown() {
  if (agent_) {
    agent_->Cancel();
    agent_.reset();
  }
  pending_reply_.reset();
  chat_ = {};
  shell_ = {};
  use_llm_ = false;
}

bool SetupChatDemo(Rml::Context* context, const AppConfig& config) {
  return ChatDemo::Instance().Setup(context, config);
}

void UpdateChatDemo() {
  ChatDemo::Instance().Update();
}

void ShutdownChatDemo() {
  ChatDemo::Instance().Shutdown();
}

} // namespace pbr
