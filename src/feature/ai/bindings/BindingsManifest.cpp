#include "feature/ai/bindings/BindingsManifest.h"

#include "common/ValueJson.h"

namespace pbr {

Roe<void> BindingsManifest::Parse(const std::string& json_text, BindingsManifest& out) {
  auto root = TryParseObject(json_text);
  if (!root) {
    return Error("Invalid bindings manifest JSON");
  }

  const Object* actions = root->getObject("actions");
  if (!actions) {
    return Error("Bindings manifest missing actions object");
  }

  out.actions_.clear();
  for (const auto& [key, value] : actions->fields()) {
    const Object* node = asObject(value);
    if (!node) {
      return Error("Action binding missing tool field: " + key);
    }
    ActionBinding binding;
    auto tool = node->getString("tool");
    if (!tool) {
      return Error("Action binding missing tool field: " + key);
    }
    binding.tool = *tool;
    if (const Object* params = node->getObject("params")) {
      binding.params = *params;
    }
    if (auto result_bind = node->getString("result_bind")) {
      binding.result_bind = *result_bind;
    }
    if (auto risk = node->getString("risk")) {
      binding.risk = *risk;
    }
    out.actions_[key] = std::move(binding);
  }
  return {};
}

const ActionBinding* BindingsManifest::Find(const std::string& action) const {
  auto it = actions_.find(action);
  if (it == actions_.end()) {
    return nullptr;
  }
  return &it->second;
}

ValidationResult BindingsManifest::Validate(const std::string& bindings_json) {
  ValidationResult result;
  BindingsManifest manifest;
  auto parse_result = Parse(bindings_json, manifest);
  if (!parse_result) {
    result.ok = false;
    result.errors.push_back(parse_result.error().message);
  }
  return result;
}

} // namespace pbr
