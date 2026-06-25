#include "bindings/ActionRouter.h"

#include "ui/DataModelHost.h"

#include <RmlUi/Core/Context.h>

namespace pbr {

ActionRouter::ActionRouter() {
  redirectLogger("ActionRouter");
}

ActionRouter& ActionRouter::Instance() {
  static ActionRouter router;
  return router;
}

void ActionRouter::Attach(Rml::Context* context) {
  context_ = context;
}

void ActionRouter::Detach() {
  context_ = nullptr;
  stubs_.clear();
}

void ActionRouter::SetManifest(const BindingsManifest& manifest) {
  manifest_ = manifest;
}

void ActionRouter::SetToolExecutor(ToolExecutor executor) {
  tool_executor_ = std::move(executor);
}

void ActionRouter::RegisterStub(const std::string& action, std::function<void()> handler) {
  stubs_[action] = std::move(handler);
}

void ActionRouter::Invoke(const std::string& action) {
  auto stub = stubs_.find(action);
  if (stub != stubs_.end()) {
    stub->second();
    return;
  }

  const ActionBinding* binding = manifest_.Find(action);
  if (!binding) {
    log().warning << "Unknown action: " << action;
    return;
  }

  if (binding->risk == "destructive" || binding->risk == "write") {
    log().warning << "Action requires confirmation: " << action;
    // Phase 3: show confirmation panel; for now log only on write without executor.
  }

  if (!tool_executor_) {
    log().warning << "No tool executor for action: " << action;
    return;
  }

  auto result = tool_executor_(binding->tool, binding->params);
  if (!binding->result_bind.empty()) {
    DataModelHost::Instance().Dirty("main", binding->result_bind);
  }
  (void)result;
}

} // namespace pbr
