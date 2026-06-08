#include "bindings/BindingsManifest.h"

namespace ppbrowser {

bool BindingsManifest::Parse(const std::string& json_text, BindingsManifest& out) {
  try {
    auto root = nlohmann::json::parse(json_text);
    if (!root.contains("actions") || !root["actions"].is_object()) {
      return false;
    }
    out.actions_.clear();
    for (auto it = root["actions"].begin(); it != root["actions"].end(); ++it) {
      ActionBinding binding;
      const auto& node = it.value();
      if (!node.contains("tool") || !node["tool"].is_string()) {
        return false;
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
    return true;
  } catch (...) {
    return false;
  }
}

const ActionBinding* BindingsManifest::Find(const std::string& action) const {
  auto it = actions_.find(action);
  if (it == actions_.end()) {
    return nullptr;
  }
  return &it->second;
}

} // namespace ppbrowser
