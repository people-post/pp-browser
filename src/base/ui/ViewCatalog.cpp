#include "base/ui/ViewCatalog.h"

#include "base/i18n/LocalizationService.h"
#include "base/platform/AssetIO.h"
#include "base/platform/IAssetLocator.h"

#include <string_view>
#include <unordered_map>

namespace pbr {

namespace {

constexpr std::string_view kIncludePrefix = "{{include:";

const std::unordered_map<std::string, std::string>& KnownKeys() {
  static const std::unordered_map<std::string, std::string> keys = {
      {"sidebar", "views/sidebar.rml"},
      {"chat", "views/chat.rml"},
      {"preview", "views/preview.rml"},
      {"dialog", "views/dialog.rml"},
      {"composer", "views/composer.rml"},
      {"nav_rail", "views/nav_rail.rml"},
      {"contacts", "views/contacts.rml"},
      {"contact_detail", "views/contact_detail.rml"},
      {"people_picker", "views/people_picker.rml"},
      {"settings", "views/settings.rml"},
      {"settings_sections", "views/settings_sections.rml"},
  };
  return keys;
}

std::string LoadRawBody(const std::string& key_or_path) {
  const std::string relative = ViewCatalog::ResolvePath(key_or_path);
  const std::string raw = ViewCatalog::LoadFile(IAssetLocator::Instance().Resolve(relative));
  if (raw.empty()) {
    return raw;
  }
  return LocalizationService::Instance().LocalizeText(raw);
}

std::string ExpandIncludes(std::string body, int depth = 0) {
  if (depth > 8) {
    return body;
  }

  for (;;) {
    const size_t start = body.find(kIncludePrefix);
    if (start == std::string::npos) {
      break;
    }
    const size_t key_start = start + kIncludePrefix.size();
    const size_t end = body.find("}}", key_start);
    if (end == std::string::npos) {
      break;
    }
    const std::string key = body.substr(key_start, end - key_start);
    const std::string fragment = ExpandIncludes(LoadRawBody(key), depth + 1);
    body.replace(start, end + 2 - start, fragment);
  }

  return body;
}

} // namespace

std::string ViewCatalog::ResolvePath(const std::string& key_or_path) {
  if (key_or_path.find('/') != std::string::npos || key_or_path.find('\\') != std::string::npos) {
    return key_or_path;
  }
  const auto it = KnownKeys().find(key_or_path);
  if (it != KnownKeys().end()) {
    return it->second;
  }
  return key_or_path;
}

std::string ViewCatalog::LoadFile(const std::string& absolute_path) {
  std::string contents;
  if (!AssetIO::ReadText(absolute_path, contents)) {
    return {};
  }
  return contents;
}

std::string ViewCatalog::LoadBody(const std::string& key_or_path) {
  return ExpandIncludes(LoadRawBody(key_or_path));
}

} // namespace pbr
