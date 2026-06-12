#pragma once

#include "app/Config.h"

#include <memory>
#include <string>
#include <vector>

namespace pbr {

enum class AgentEventType {
  LoadingChanged,
  ToolActivity,
  AssistantReady,
  Error,
};

struct AgentEvent {
  AgentEventType type = AgentEventType::LoadingChanged;
  bool loading = false;
  std::string tool_name;
  std::string status;
  std::string text;
  std::string message;
};

class AgentSession {
public:
  AgentSession();
  ~AgentSession();

  AgentSession(const AgentSession&) = delete;
  AgentSession& operator=(const AgentSession&) = delete;

  void Configure(const AppConfig& config);
  void Submit(const std::string& user_text);
  void PollEvents(std::vector<AgentEvent>& out);
  void Cancel();

  bool IsConfigured() const;

private:
  struct Impl;

  static void ConfigureOnIO(const std::shared_ptr<Impl>& state);
  static void StartTurn(const std::shared_ptr<Impl>& state);
  static void RunLlmStep(const std::shared_ptr<Impl>& state);
  static void HandleLlmResponse(const std::shared_ptr<Impl>& state, const struct ChatCompletionResponse& response);
  static void PushEvent(const std::shared_ptr<Impl>& state, AgentEvent event);
  static void PushLoading(const std::shared_ptr<Impl>& state, bool loading);
  static void PushToolActivity(const std::shared_ptr<Impl>& state, const std::string& tool_name,
                               const std::string& status);
  static void PushAssistantReady(const std::shared_ptr<Impl>& state, const std::string& text);
  static void PushError(const std::shared_ptr<Impl>& state, const std::string& message);
  static void FinishTurn(const std::shared_ptr<Impl>& state);

  std::shared_ptr<Impl> impl_;
};

} // namespace pbr
