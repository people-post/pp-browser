#include "demo/ChatDemo.h"

#include "agent/LlmClient.h"
#include "agent/PromptBuilder.h"
#include "agent/StructuredTextParser.h"
#include "app/Application.h"
#include "app/InputCoordinator.h"
#include "ui/DataModelHost.h"
#include "ui/DocumentLoader.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
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
  // Parser emits bubble inner content only; suggestion buttons bind via data-for in chat_dialog.rml.
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
  DataModelHost::Instance().Dirty("chat", "messages");
  DataModelHost::Instance().Dirty("chat", "loading");
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
  DirtyChat();

  if (!llm_) {
    log().debug << "Using mock assistant response";
    // Defer until Update() so the click event finishes before the message list rebuilds.
    pending_reply_ = PendingReply{MockAssistantRespond(trimmed), false};
    return;
  }

  log().info << "Sending message to LLM";
  llm_job_ = std::make_unique<LlmJob>();
  ChatDemo::LlmJob* job = llm_job_.get();
  const std::string system_prompt = PromptBuilder::BuildChatSystemPrompt();
  LlmClient client = *llm_;

  job->future = std::async(std::launch::async, [this, job, client, system_prompt, trimmed]() {
    auto result = client.Complete(system_prompt, trimmed);
    std::lock_guard lock(job->mutex);
    if (!result) {
      log().error << "LLM request failed: " << result.error().message;
      job->error = result.error().message;
    } else {
      job->response = result.value();
    }
  });
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
  }
  chat_.loading = false;
  DirtyChat();
}

bool ChatDemo::Setup(Rml::Context* context, const AppConfig& config) {
  if (!context) {
    return false;
  }

  chat_ = {};
  pending_reply_.reset();
  llm_job_.reset();
  llm_.emplace(config.llm);
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
        ctor.Bind("loading", &ChatDemo::Instance().chat_.loading);
        ctor.Bind("messages", &ChatDemo::Instance().chat_.messages);
        ctor.BindEventCallback("send_message", &ChatDemo::SendMessageCallback);
        ctor.BindEventCallback("send_suggestion", &ChatDemo::SendSuggestionCallback);
      })) {
    return false;
  }

  return DocumentLoader::LoadFile(context, Application::AssetsPath("samples/chat_dialog.rml")) != nullptr;
}

void ChatDemo::Update() {
  if (pending_reply_) {
    PendingReply reply = std::move(*pending_reply_);
    pending_reply_.reset();
    FinishAssistantReply(reply.output, reply.from_llm);
  }

  if (!llm_job_ || !chat_.loading) {
    return;
  }

  if (llm_job_->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return;
  }

  llm_job_->future.get();

  std::optional<std::string> response;
  std::optional<std::string> error;
  {
    std::lock_guard lock(llm_job_->mutex);
    response = llm_job_->response;
    error = llm_job_->error;
  }
  llm_job_.reset();

  if (error) {
    log().error << "LLM job error: " << *error;
    chat_.messages.push_back({Rml::String("assistant"), ErrorMessageRml(*error), {}});
    chat_.loading = false;
    DirtyChat();
    return;
  }

  FinishAssistantReply(*response, true);
}

void ChatDemo::Shutdown() {
  if (llm_job_ && llm_job_->future.valid()) {
    llm_job_->future.wait();
  }
  pending_reply_.reset();
  llm_job_.reset();
  llm_.reset();
  chat_ = {};
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
