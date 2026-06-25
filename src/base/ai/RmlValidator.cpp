#include "base/ai/RmlValidator.h"

#include "feature/ai/bindings/BindingsManifest.h"

#include <algorithm>
#include <cctype>

namespace pbr {

namespace {

bool ContainsInsensitive(const std::string& haystack, const std::string& needle) {
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
  return it != haystack.end();
}

ValidationResult ValidateForbiddenPatterns(const std::string& rml) {
  ValidationResult result;
  if (ContainsInsensitive(rml, "<script")) {
    result.ok = false;
    result.errors.push_back("script elements are forbidden");
  }
  if (ContainsInsensitive(rml, "javascript:")) {
    result.ok = false;
    result.errors.push_back("javascript: URLs are forbidden");
  }
  if (ContainsInsensitive(rml, "onclick=")) {
    result.ok = false;
    result.errors.push_back("inline event handlers are forbidden");
  }
  return result;
}

} // namespace

ValidationResult RmlValidator::ValidateRml(const std::string& rml) {
  ValidationResult result = ValidateForbiddenPatterns(rml);
  if (rml.find("<rml>") == std::string::npos) {
    result.ok = false;
    result.errors.push_back("Missing <rml> root element");
  }
  return result;
}

ValidationResult RmlValidator::ValidateFragment(const std::string& rml_fragment) {
  return ValidateForbiddenPatterns(rml_fragment);
}

ValidationResult RmlValidator::ValidateBindings(const std::string& bindings_json) {
  ValidationResult result;
  BindingsManifest manifest;
  auto parse_result = BindingsManifest::Parse(bindings_json, manifest);
  if (!parse_result) {
    result.ok = false;
    result.errors.push_back(parse_result.error().message);
  }
  return result;
}

} // namespace pbr
