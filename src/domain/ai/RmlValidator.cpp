#include "domain/ai/RmlValidator.h"

#include <algorithm>
#include <cctype>
#include <regex>

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

ValidationResult RmlValidator::ValidateRcss(const std::string& rcss) {
  ValidationResult result;
  static const std::regex hex_color(R"re(#([0-9a-fA-F]{3,8}))re");
  if (std::regex_search(rcss, hex_color)) {
    result.ok = false;
    result.errors.push_back("raw hex colors are forbidden in AI RCSS; use design-system classes");
  }
  if (rcss.find("@media") != std::string::npos) {
    result.ok = false;
    result.errors.push_back("@media rules are forbidden in AI RCSS");
  }
  return result;
}

} // namespace pbr
