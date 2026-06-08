#include "bindings/ActionRouter.h"

#include "ui/DataModelHost.h"

#include <RmlUi/Core/Context.h>
#include <iostream>

namespace ppbrowser {

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
    std::cerr << "Unknown action: " << action << '\n';
    return;
  }

  if (binding->risk == "destructive" || binding->risk == "write") {
    std::cerr << "Action requires confirmation: " << action << '\n';
    // Phase 3: show confirmation panel; for now log only on write without executor.
  }

  if (!tool_executor_) {
    std::cerr << "No tool executor for action: " << action << '\n';
    return;
  }

  auto result = tool_executor_(binding->tool, binding->params);
  if (!binding->result_bind.empty()) {
    DataModelHost::Instance().Dirty("main", binding->result_bind);
  }
  (void)result;
}

} // namespace ppbrowser
