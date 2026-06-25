#include "feature/ai/bindings/BindingsManifest.h"

namespace pbr {

Roe<void> BindingsManifest::Parse(const std::string& json_text, BindingsManifest& out) {
  auto root = nlohmann::json::parse(json_text, nullptr, false);
  if (root.is_discarded()) {
    return Error("Invalid bindings manifest JSON");
  }

  if (!root.contains("actions") || !root["actions"].is_object()) {
    return Error("Bindings manifest missing actions object");
  }

  out.actions_.clear();
  for (auto it = root["actions"].begin(); it != root["actions"].end(); ++it) {
    ActionBinding binding;
    const auto& node = it.value();
    if (!node.contains("tool") || !node["tool"].is_string()) {
      return Error("Action binding missing tool field: " + it.key());
    }
    binding.tool = node["tool"].get<std::string>();
    if (node.contains("params")) {
      binding.params = node["params"];
    }
    if (node.contains("result_bind") && node["result_bind"].is_string()) {
      binding.result_bind = node["result_bind"].get<std::string>();
    }
    if (node.contains("risk") && node["risk"].is_string()) {
      binding.risk = node["risk"].get<std::string>();
    }
    out.actions_[it.key()] = std::move(binding);
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
