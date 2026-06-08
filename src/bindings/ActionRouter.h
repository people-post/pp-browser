#pragma once

#include "bindings/BindingsManifest.h"

#include <RmlUi/Core/Types.h>
#include <functional>
#include <string>
#include <unordered_map>

namespace Rml {
class Context;
}

namespace ppbrowser {

using ToolExecutor = std::function<nlohmann::json(const std::string& tool, const nlohmann::json& params)>;

class ActionRouter {
public:
  static ActionRouter& Instance();

  void Attach(Rml::Context* context);
  void Detach();

  void SetManifest(const BindingsManifest& manifest);
  void SetToolExecutor(ToolExecutor executor);
  void RegisterStub(const std::string& action, std::function<void()> handler);

  void Invoke(const std::string& action);

private:
  ActionRouter() = default;

  Rml::Context* context_ = nullptr;
  BindingsManifest manifest_;
  ToolExecutor tool_executor_;
  std::unordered_map<std::string, std::function<void()>> stubs_;
};

} // namespace ppbrowser
