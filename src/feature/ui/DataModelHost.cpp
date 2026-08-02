#include "feature/ui/DataModelHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

namespace pbr {

DataModelHost& DataModelHost::Instance() {
  static DataModelHost host;
  return host;
}

bool DataModelHost::Register(Rml::Context* context, const std::string& name, DataModelSetupFn setup) {
  if (!context || !setup) {
    return false;
  }
  auto constructor = context->CreateDataModel(name);
  if (!constructor) {
    return false;
  }
  setup(constructor);
  models_[name] = {constructor.GetModelHandle()};
  return true;
}

Rml::DataModelHandle DataModelHost::Get(const std::string& name) const {
  auto it = models_.find(name);
  if (it == models_.end()) {
    return {};
  }
  return it->second.handle;
}

void DataModelHost::Dirty(const std::string& model, const std::string& key) {
  auto handle = Get(model);
  if (handle) {
    handle.DirtyVariable(key);
  }
}

void DataModelHost::DirtyAll(const std::string& model) {
  auto handle = Get(model);
  if (handle) {
    handle.DirtyAllVariables();
  }
}

void DataModelHost::Clear() {
  models_.clear();
}

} // namespace pbr
