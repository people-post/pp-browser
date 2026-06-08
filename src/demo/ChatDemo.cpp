#include "demo/ChatDemo.h"

#include "agent/LlmClient.h"
#include "agent/PromptBuilder.h"
#include "agent/StructuredTextParser.h"
#include "app/Application.h"
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

namespace ppbrowser {

namespace {

struct ChatMessage {
  Rml::String role;
  Rml::String content_rml;
};

struct ChatState {
  Rml::String draft;
  std::vector<ChatMessage> messages;
  bool loading = false;
};

struct LlmJob {
  std::mutex mutex;
  std::optional<std::string> response;
  std::optional<std::string> error;
  std::future<void> future;
};

ChatState g_chat;
std::optional<LlmClient> g_llm;
std::unique_ptr<LlmJob> g_llm_job;

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

Rml::String AssistantMessageRml(const std::string& rml) {
  return Rml::String(rml.c_str());
}

Rml::String ErrorMessageRml(const std::string& message) {
  return Rml::String(("<div class=\"bubble bubble-assistant error\" selectable=\"text\"><p>" +
                      StructuredTextParser::EscapeText(message) + "</p></div>")
                         .c_str());
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
          "Try help, list, or code for sample replies",
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

  return R"({
    "blocks": [
      { "type": "paragraph", "text": "Thanks for your message. This is a mock assistant response." },
      { "type": "paragraph", "text": "Try typing help, list, or code to see other structured reply formats." }
    ]
  })";
}

void DirtyChat() {
  DataModelHost::Instance().Dirty("chat", "draft");
  DataModelHost::Instance().Dirty("chat", "messages");
  DataModelHost::Instance().Dirty("chat", "loading");
}

void FinishAssistantReply(const std::string& raw_output, bool from_llm) {
  auto parsed = from_llm ? StructuredTextParser::ParseFromLlmOutput(raw_output)
                         : StructuredTextParser::ParseBlocksJson(raw_output);
  if (!parsed.ok) {
    g_chat.messages.push_back({Rml::String("assistant"), ErrorMessageRml(parsed.error)});
  } else {
    g_chat.messages.push_back({Rml::String("assistant"), AssistantMessageRml(parsed.rml)});
  }
  g_chat.loading = false;
  DirtyChat();
}

void SendMessage(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& /*args*/) {
  if (g_chat.loading) {
    return;
  }

  const std::string text = Trim(g_chat.draft.c_str());
  if (text.empty()) {
    return;
  }

  g_chat.messages.push_back({Rml::String("user"), UserMessageRml(text)});
  g_chat.draft = "";
  g_chat.loading = true;
  DirtyChat();

  if (!g_llm) {
    FinishAssistantReply(MockAssistantRespond(text), false);
    return;
  }

  g_llm_job = std::make_unique<LlmJob>();
  LlmJob* job = g_llm_job.get();
  const std::string system_prompt = PromptBuilder::BuildChatSystemPrompt();
  LlmClient client = *g_llm;

  job->future = std::async(std::launch::async, [job, client, system_prompt, text]() {
    try {
      const std::string raw = client.Complete(system_prompt, text);
      std::lock_guard lock(job->mutex);
      job->response = raw;
    } catch (const std::exception& e) {
      std::lock_guard lock(job->mutex);
      job->error = e.what();
    }
  });
}

} // namespace

bool SetupChatDemo(Rml::Context* context, const AppConfig& config) {
  if (!context) {
    return false;
  }

  g_chat = {};
  g_llm_job.reset();
  g_llm.emplace(config.llm);

  DataModelHost::Instance().Clear();

  if (!DataModelHost::Instance().Register(context, "chat", [](Rml::DataModelConstructor& ctor) {
        if (auto msg_handle = ctor.RegisterStruct<ChatMessage>()) {
          msg_handle.RegisterMember("role", &ChatMessage::role);
          msg_handle.RegisterMember("content_rml", &ChatMessage::content_rml);
        }
        ctor.RegisterArray<std::vector<ChatMessage>>();
        ctor.Bind("draft", &g_chat.draft);
        ctor.Bind("loading", &g_chat.loading);
        ctor.Bind("messages", &g_chat.messages);
        ctor.BindEventCallback("send_message", &SendMessage);
      })) {
    return false;
  }

  return DocumentLoader::LoadFile(context, Application::AssetsPath("samples/chat_dialog.rml")) != nullptr;
}

void UpdateChatDemo() {
  if (!g_llm_job || !g_chat.loading) {
    return;
  }

  if (g_llm_job->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return;
  }

  g_llm_job->future.get();

  std::optional<std::string> response;
  std::optional<std::string> error;
  {
    std::lock_guard lock(g_llm_job->mutex);
    response = g_llm_job->response;
    error = g_llm_job->error;
  }
  g_llm_job.reset();

  if (error) {
    g_chat.messages.push_back({Rml::String("assistant"), ErrorMessageRml(*error)});
    g_chat.loading = false;
    DirtyChat();
    return;
  }

  FinishAssistantReply(*response, true);
}

void ShutdownChatDemo() {
  if (g_llm_job && g_llm_job->future.valid()) {
    g_llm_job->future.wait();
  }
  g_llm_job.reset();
  g_llm.reset();
  g_chat = {};
}

} // namespace ppbrowser
