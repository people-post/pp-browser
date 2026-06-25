#pragma once

#include <string>
#include <vector>

namespace pbr {

struct ValidationResult {
  bool ok = true;
  std::vector<std::string> errors;
};

class RmlValidator {
public:
  static ValidationResult ValidateRml(const std::string& rml);
  static ValidationResult ValidateFragment(const std::string& rml_fragment);
};

} // namespace pbr
