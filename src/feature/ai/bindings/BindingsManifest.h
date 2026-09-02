#pragma once

#include "domain/ai/RmlValidator.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <string>
#include <unordered_map>

namespace pbr {

struct ActionBinding {
  std::string tool;
  Object params;
  std::string result_bind;
  std::string risk = "read"; // read | write | destructive
};

class BindingsManifest {
public:
  static Roe<void> Parse(const std::string& json_text, BindingsManifest& out);
  static ValidationResult Validate(const std::string& bindings_json);

  const ActionBinding* Find(const std::string& action) const;

private:
  std::unordered_map<std::string, ActionBinding> actions_;
};

} // namespace pbr
