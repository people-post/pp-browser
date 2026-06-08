#pragma once

#include "agent/LlmClient.h"
#include "app/Config.h"
#include "common/Module.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

class ChatDemo : public Module {
public:
  static ChatDemo& Instance();

  bool Setup(Rml::Context* context, const AppConfig& config);
  void Update();
  void Shutdown();

private:
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

  ChatDemo();

  static void SendMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void OnSendMessage();
  void FinishAssistantReply(const std::string& raw_output, bool from_llm);

  ChatState chat_;
  std::optional<LlmClient> llm_;
  std::unique_ptr<LlmJob> llm_job_;
};

bool SetupChatDemo(Rml::Context* context, const AppConfig& config);
void UpdateChatDemo();
void ShutdownChatDemo();

} // namespace pbr
