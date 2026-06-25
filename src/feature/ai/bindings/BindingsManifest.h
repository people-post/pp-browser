#pragma once

#include "common/Error.h"

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace pbr {

struct ActionBinding {
  std::string tool;
  nlohmann::json params;
  std::string result_bind;
  std::string risk = "read"; // read | write | destructive
};

class BindingsManifest {
public:
  static Roe<void> Parse(const std::string& json_text, BindingsManifest& out);

  const ActionBinding* Find(const std::string& action) const;

private:
  std::unordered_map<std::string, ActionBinding> actions_;
};

} // namespace pbr
