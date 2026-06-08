#include "agent/RmlValidator.h"

#include "bindings/BindingsManifest.h"

#include <algorithm>
#include <cctype>

namespace pbr {

namespace {

bool ContainsInsensitive(const std::string& haystack, const std::string& needle) {
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
  return it != haystack.end();
}

} // namespace

ValidationResult RmlValidator::ValidateRml(const std::string& rml) {
  ValidationResult result;
  if (rml.find("<rml>") == std::string::npos) {
    result.ok = false;
    result.errors.push_back("Missing <rml> root element");
  }
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

ValidationResult RmlValidator::ValidateBindings(const std::string& bindings_json) {
  ValidationResult result;
  BindingsManifest manifest;
  if (!BindingsManifest::Parse(bindings_json, manifest)) {
    result.ok = false;
    result.errors.push_back("Invalid bindings manifest JSON");
  }
  return result;
}

} // namespace pbr
