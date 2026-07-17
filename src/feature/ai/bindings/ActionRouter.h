#pragma once

#include "feature/ai/bindings/BindingsManifest.h"
#include "common/Module.h"

#include <RmlUi/Core/Types.h>
#include <functional>
#include <string>
#include <unordered_map>

namespace Rml {
class Context;
}

namespace pbr {

using ToolExecutor = std::function<nlohmann::json(const std::string& tool, const nlohmann::json& params)>;

class ActionRouter : public Module {
public:
  static ActionRouter& Instance();

  void Attach(Rml::Context* context);
  void Detach();

  void SetManifest(const BindingsManifest& manifest);
  void SetToolExecutor(ToolExecutor executor);
  void SetModelDirtyCallback(std::function<void(const std::string& model, const std::string& binding)> callback);
  void RegisterStub(const std::string& action, std::function<void()> handler);

  void Invoke(const std::string& action);

private:
  ActionRouter();

  Rml::Context* context_ = nullptr;
  BindingsManifest manifest_;
  ToolExecutor tool_executor_;
  std::function<void(const std::string& model, const std::string& binding)> model_dirty_callback_;
  std::unordered_map<std::string, std::function<void()>> stubs_;
};

} // namespace pbr
