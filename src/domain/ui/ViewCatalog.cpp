#include "domain/ui/ViewCatalog.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/AssetIO.h"
#include "foundation/platform/IAssetLocator.h"

#include <string_view>
#include <unordered_map>

namespace pbr {

namespace {

constexpr std::string_view kIncludePrefix = "{{include:";

const std::unordered_map<std::string, std::string>& KnownKeys() {
  static const std::unordered_map<std::string, std::string> keys = {
      {"sidebar", "views/sidebar.rml"},
      {"chat", "views/chat.rml"},
      {"home", "views/home.rml"},
      {"preview", "views/preview.rml"},
      {"dialog", "views/dialog.rml"},
      {"composer", "views/composer.rml"},
      {"nav_rail", "views/nav_rail.rml"},
      {"contacts", "views/contacts.rml"},
      {"contact_detail", "views/contact_detail.rml"},
      {"people_picker", "views/people_picker.rml"},
      {"emoji_picker", "views/emoji_picker.rml"},
      {"settings", "views/settings.rml"},
      {"settings_detail", "views/settings_detail.rml"},
      {"settings_sections", "views/settings_sections.rml"},
      {"settings_section_profile", "views/settings_section_profile.rml"},
      {"settings_section_llm", "views/settings_section_llm.rml"},
      {"settings_section_integrations", "views/settings_section_integrations.rml"},
      {"settings_section_network", "views/settings_section_network.rml"},
      {"settings_section_appearance", "views/settings_section_appearance.rml"},
      {"settings_section_security", "views/settings_section_security.rml"},
      {"settings_section_storage", "views/settings_section_storage.rml"},
      {"settings_section_about", "views/settings_section_about.rml"},
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

std::unordered_map<std::string, std::string>& BodyCache() {
  static std::unordered_map<std::string, std::string> cache;
  return cache;
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
  const std::string cache_key = ResolvePath(key_or_path);
  auto& cache = BodyCache();
  if (const auto it = cache.find(cache_key); it != cache.end()) {
    return it->second;
  }
  std::string body = ExpandIncludes(LoadRawBody(key_or_path));
  // Do not cache empty (missing asset) so a later successful read can populate.
  if (!body.empty()) {
    cache.emplace(cache_key, body);
  }
  return body;
}

void ViewCatalog::ClearCache() {
  BodyCache().clear();
}

} // namespace pbr
