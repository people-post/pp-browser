#include "demo/ChatDemo.h"

#include "agent/StructuredTextParser.h"
#include "app/Application.h"
#include "ui/DataModelHost.h"
#include "ui/DocumentLoader.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <algorithm>
#include <cctype>
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

ChatState g_chat;

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

  const std::string mock_json = MockAssistantRespond(text);
  auto parsed = StructuredTextParser::ParseBlocksJson(mock_json);

  g_chat.messages.push_back({Rml::String("assistant"), Rml::String(parsed.rml.c_str())});
  g_chat.loading = false;
  DirtyChat();
}

} // namespace

bool SetupChatDemo(Rml::Context* context) {
  if (!context) {
    return false;
  }

  g_chat = {};

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

} // namespace ppbrowser
