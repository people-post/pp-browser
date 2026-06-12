#include "demo/ChatDemo.h"

#include "agent/StructuredTextParser.h"
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
#include <string>
#include <vector>

namespace pbr {

namespace {

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

Rml::String ErrorMessageRml(const std::string& message) {
  return Rml::String(("<p class=\"error\">" + StructuredTextParser::EscapeText(message) + "</p>").c_str());
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
  DataModelHost::Instance().Dirty("chat", "messages");
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

void ChatDemo::SendUserText(const std::string& text) {
  const std::string trimmed = Trim(text);
  if (trimmed.empty() || chat_.loading) {
    return;
  }

  chat_.messages.push_back({Rml::String("user"), UserMessageRml(trimmed), {}});
  chat_.loading = true;
  chat_.status = "";
  DirtyChat();

  if (!use_llm_) {
    log().debug << "Using mock assistant response";
    pending_reply_ = PendingReply{MockAssistantRespond(trimmed), false};
    return;
  }

  log().info << "Submitting message to agent session";
  agent_->Submit(trimmed);
}

void ChatDemo::FinishAssistantReply(const std::string& raw_output, bool from_llm) {
  auto parsed = from_llm ? StructuredTextParser::ParseFromLlmOutput(raw_output)
                         : StructuredTextParser::ParseBlocksJson(raw_output);
  if (!parsed.ok) {
    log().warning << "Failed to parse assistant reply: " << parsed.error;
    log().warning << "AI response: " << raw_output;
    chat_.messages.push_back({Rml::String("assistant"), ErrorMessageRml(parsed.error), {}});
  } else {
    std::vector<ChatSuggestion> suggestions;
    suggestions.reserve(parsed.suggestions.size());
    for (const ParsedSuggestion& suggestion : parsed.suggestions) {
      suggestions.push_back({Rml::String(suggestion.label.c_str()), Rml::String(suggestion.message.c_str())});
    }
    chat_.messages.push_back({Rml::String("assistant"), AssistantBubbleRml(parsed.rml), std::move(suggestions)});
    shell_.preview_rml = Rml::String(parsed.rml.c_str());
    DirtyShell();
  }
  chat_.loading = false;
  chat_.status = "";
  DirtyChat();
}

void ChatDemo::HandleAgentEvent(const AgentEvent& event) {
  switch (event.type) {
  case AgentEventType::LoadingChanged:
    chat_.loading = event.loading;
    if (!event.loading) {
      chat_.status = "";
    }
    DirtyChat();
    break;
  case AgentEventType::ToolActivity:
    if (event.status == "running") {
      chat_.status = Rml::String(("Running " + event.tool_name + "...").c_str());
      DirtyChat();
    } else if (event.status == "done" && chat_.status.empty()) {
      chat_.status = Rml::String(("Finished " + event.tool_name).c_str());
      DirtyChat();
    }
    break;
  case AgentEventType::AssistantReady:
    FinishAssistantReply(event.text, true);
    break;
  case AgentEventType::Error:
    log().error << "Agent session error: " << event.message;
    chat_.messages.push_back({Rml::String("assistant"), ErrorMessageRml(event.message), {}});
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
  shell_.sessions = {
      {Rml::String("New chat"), Rml::String("Ask anything...")},
      {Rml::String("Help"), Rml::String("Try help, list, code, or button")},
  };
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
        if (auto msg_handle = ctor.RegisterStruct<ChatMessage>()) {
          msg_handle.RegisterMember("role", &ChatMessage::role);
          msg_handle.RegisterMember("content_rml", &ChatMessage::content_rml);
          msg_handle.RegisterMember("suggestions", &ChatMessage::suggestions);
        }
        ctor.RegisterArray<std::vector<ChatMessage>>();
        ctor.Bind("draft", &ChatDemo::Instance().chat_.draft);
        ctor.Bind("status", &ChatDemo::Instance().chat_.status);
        ctor.Bind("loading", &ChatDemo::Instance().chat_.loading);
        ctor.Bind("messages", &ChatDemo::Instance().chat_.messages);
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
    FinishAssistantReply(reply.output, reply.from_llm);
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
