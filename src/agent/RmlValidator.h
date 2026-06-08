#pragma once

#include <string>
#include <vector>

namespace ppbrowser {

struct ValidationResult {
  bool ok = true;
  std::vector<std::string> errors;
};

class RmlValidator {
public:
  static ValidationResult ValidateRml(const std::string& rml);
  static ValidationResult ValidateBindings(const std::string& bindings_json);
};

} // namespace ppbrowser
