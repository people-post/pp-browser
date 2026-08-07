#include "base/i18n/LocalizationService.h"

#include "base/platform/AssetIO.h"
#include "base/platform/IAssetLocator.h"

#if !defined(PP_BROWSER_HEADLESS)
#include <SDL3/SDL_locale.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

bool LooksLikeLocaleJson(const nlohmann::json& root) {
  return root.is_object() && root.contains("locale") && root.contains("strings") && root["strings"].is_object();
}

} // namespace

LocalizationService& LocalizationService::Instance() {
  static LocalizationService service;
  return service;
}

LocalizationService::Prefs LocalizationService::Project(const ProfilePreferences& prefs) {
  return {.language = prefs.language};
}

void LocalizationService::Apply(const Prefs& prefs) {
  SetPreferredLanguage(prefs.language);
}

Roe<void> LocalizationService::LoadFromAssets(const std::string& assets_root) {
  catalogs_.clear();
  available_.clear();

  auto try_load_locale_file = [&](const std::string& absolute_or_relative) -> bool {
    std::string text;
    if (!AssetIO::ReadText(absolute_or_relative, text)) {
      return false;
    }
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !LooksLikeLocaleJson(root)) {
      return false;
    }
    std::unordered_map<std::string, std::string> strings;
    for (const auto& [key, value] : root["strings"].items()) {
      if (value.is_string()) {
        strings[key] = value.get<std::string>();
      }
    }
    const std::string locale = root["locale"].get<std::string>();
    catalogs_[locale] = std::move(strings);
    return true;
  };

  // Prefer explicit files via the same Resolve() path as ViewCatalog (works on iOS
  // packaged builds). directory_iterator + absolute SDL reads has been failing to
  // populate catalogs on simulator even when locales/*.json are present in the bundle.
  for (const char* tag : {"en", "zh-Hans"}) {
    const std::string relative = std::string("locales/") + tag + ".json";
    const std::string resolved = IAssetLocator::Instance().Resolve(relative);
    if (try_load_locale_file(resolved)) {
      continue;
    }
    const std::string under_root =
        (std::filesystem::path(assets_root) / relative).lexically_normal().string();
    if (try_load_locale_file(under_root)) {
      continue;
    }
    (void)try_load_locale_file(relative);
  }

  // Also pick up any extra locale JSON dropped into the locales dir (desktop / future).
  const std::filesystem::path locales_dir = std::filesystem::path(assets_root) / "locales";
  std::error_code ec;
  if (std::filesystem::is_directory(locales_dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(locales_dir, ec)) {
      if (ec || !entry.is_regular_file()) {
        continue;
      }
      if (entry.path().extension() != ".json") {
        continue;
      }
      (void)try_load_locale_file(entry.path().string());
    }
  }

  if (catalogs_.find("en") == catalogs_.end()) {
    return Error("Missing required locale catalog: en");
  }

  // Stable order for picker: English first, then Chinese, then others alpha.
  available_.push_back({.tag = "en", .native_name_key = "settings.language.en"});
  if (catalogs_.count("zh-Hans")) {
    available_.push_back({.tag = "zh-Hans", .native_name_key = "settings.language.zh_hans"});
  }
  std::vector<std::string> extras;
  for (const auto& [tag, _] : catalogs_) {
    if (tag == "en" || tag == "zh-Hans") {
      continue;
    }
    extras.push_back(tag);
  }
  std::sort(extras.begin(), extras.end());
  for (const auto& tag : extras) {
    available_.push_back({.tag = tag, .native_name_key = "settings.language." + tag});
  }

  ResolveAndNotify(false);
  return {};
}

void LocalizationService::SetPreferredLanguage(std::string pref) {
  if (pref.empty()) {
    pref = "system";
  }
  preferred_ = std::move(pref);
  ResolveAndNotify(true);
}

void LocalizationService::SetSystemLocalesForTest(std::vector<std::string> locales) {
  system_locales_override_ = std::move(locales);
  use_system_locales_override_ = true;
  ResolveAndNotify(true);
}

void LocalizationService::ClearSystemLocalesForTest() {
  system_locales_override_.clear();
  use_system_locales_override_ = false;
  ResolveAndNotify(true);
}

std::string LocalizationService::Tr(std::string_view key) const {
  return Lookup(key);
}

std::string LocalizationService::Tr(std::string_view key, const std::map<std::string, std::string>& args) const {
  return Interpolate(Lookup(key), args);
}

std::vector<LocaleInfo> LocalizationService::AvailableLocales() const {
  return available_;
}

std::string LocalizationService::LocalizeText(std::string_view input) const {
  std::string out;
  out.reserve(input.size());
  size_t i = 0;
  while (i < input.size()) {
    if (i + 7 < input.size() && input.compare(i, 7, "{{i18n:") == 0) {
      const size_t key_start = i + 7;
      const size_t key_end = input.find("}}", key_start);
      if (key_end != std::string_view::npos) {
        const std::string_view key = input.substr(key_start, key_end - key_start);
        out += Lookup(key);
        i = key_end + 2;
        continue;
      }
    }
    out.push_back(input[i]);
    ++i;
  }
  return out;
}

void LocalizationService::AddLanguageChangeListener(std::function<void(const std::string& resolved)> listener) {
  if (listener) {
    listeners_.push_back(std::move(listener));
  }
}

std::string LocalizationService::LanguageDisplayLabel(std::string_view pref) const {
  if (pref.empty() || pref == "system") {
    return Tr("settings.language.system");
  }
  for (const auto& info : available_) {
    if (info.tag == pref) {
      // Native self-name: prefer the language's own catalog, then English, then key.
      if (const auto cat = catalogs_.find(info.tag); cat != catalogs_.end()) {
        if (const auto it = cat->second.find(info.native_name_key); it != cat->second.end()) {
          return it->second;
        }
      }
      return Tr(info.native_name_key);
    }
  }
  return std::string(pref);
}

void LocalizationService::ResolveAndNotify(bool notify) {
  const std::string previous = resolved_;
  resolved_ = ResolvePreferred(preferred_);
  if (notify && resolved_ != previous) {
    for (const auto& listener : listeners_) {
      listener(resolved_);
    }
  }
}

std::string LocalizationService::ResolvePreferred(const std::string& pref) const {
  if (pref != "system") {
    const std::string normalized = NormalizeTag(pref);
    if (catalogs_.count(normalized)) {
      return normalized;
    }
    const std::string primary = PrimaryLanguage(normalized);
    if (primary == "zh" && catalogs_.count("zh-Hans")) {
      return "zh-Hans";
    }
    for (const auto& [tag, _] : catalogs_) {
      if (PrimaryLanguage(tag) == primary) {
        return tag;
      }
    }
    return catalogs_.count("en") ? "en" : resolved_;
  }

  for (const auto& candidate : PreferredSystemLocales()) {
    const std::string normalized = NormalizeTag(candidate);
    if (catalogs_.count(normalized)) {
      return normalized;
    }
    const std::string primary = PrimaryLanguage(normalized);
    if (primary == "zh" && catalogs_.count("zh-Hans")) {
      return "zh-Hans";
    }
    for (const auto& [tag, _] : catalogs_) {
      if (PrimaryLanguage(tag) == primary) {
        return tag;
      }
    }
  }
  return "en";
}

std::vector<std::string> LocalizationService::PreferredSystemLocales() const {
  if (use_system_locales_override_) {
    return system_locales_override_;
  }

  std::vector<std::string> out;
#if defined(PP_BROWSER_HEADLESS)
  // pp-node / headless: no SDL — honor POSIX locale env (first non-empty / non-C).
  for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* value = std::getenv(key);
    if (value == nullptr || value[0] == '\0') {
      continue;
    }
    std::string tag = value;
    // Drop encoding / modifier: en_US.UTF-8 → en_US
    const auto dot = tag.find('.');
    if (dot != std::string::npos) {
      tag.resize(dot);
    }
    const auto at = tag.find('@');
    if (at != std::string::npos) {
      tag.resize(at);
    }
    if (tag == "C" || tag == "POSIX") {
      continue;
    }
    out.push_back(NormalizeTag(tag));
    break;
  }
#else
  int count = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&count);
  if (locales == nullptr) {
    return out;
  }
  for (int i = 0; i < count; ++i) {
    if (locales[i] == nullptr || locales[i]->language == nullptr) {
      continue;
    }
    std::string tag = locales[i]->language;
    if (locales[i]->country != nullptr && locales[i]->country[0] != '\0') {
      tag.push_back('-');
      tag += locales[i]->country;
    }
    out.push_back(std::move(tag));
  }
  SDL_free(locales);
#endif
  return out;
}

std::string LocalizationService::Lookup(std::string_view key) const {
  const std::string key_str(key);
  if (const auto cat = catalogs_.find(resolved_); cat != catalogs_.end()) {
    if (const auto it = cat->second.find(key_str); it != cat->second.end()) {
      return it->second;
    }
  }
  if (resolved_ != "en") {
    if (const auto en = catalogs_.find("en"); en != catalogs_.end()) {
      if (const auto it = en->second.find(key_str); it != en->second.end()) {
        return it->second;
      }
    }
  }
  return key_str;
}

std::string LocalizationService::Interpolate(std::string templ, const std::map<std::string, std::string>& args) {
  for (const auto& [name, value] : args) {
    const std::string token = "{" + name + "}";
    size_t pos = 0;
    while ((pos = templ.find(token, pos)) != std::string::npos) {
      templ.replace(pos, token.size(), value);
      pos += value.size();
    }
  }
  return templ;
}

std::string LocalizationService::NormalizeTag(std::string_view tag) {
  std::string out;
  out.reserve(tag.size());
  for (char ch : tag) {
    if (ch == '_') {
      out.push_back('-');
    } else {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }
  // Restore canonical casing for known tags.
  if (out == "zh-hans" || out == "zh-cn" || out == "zh-sg") {
    return "zh-Hans";
  }
  if (out == "zh-hant" || out == "zh-tw" || out == "zh-hk") {
    return "zh-Hant";
  }
  if (out.size() == 2) {
    return out;
  }
  // language-REGION → language-Region for common forms; keep simple for v1.
  const auto dash = out.find('-');
  if (dash != std::string::npos && dash + 1 < out.size()) {
    std::string lang = out.substr(0, dash);
    std::string rest = out.substr(dash + 1);
    if (rest == "hans") {
      return "zh-Hans";
    }
    if (rest == "hant") {
      return "zh-Hant";
    }
    for (char& ch : rest) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return lang + "-" + rest;
  }
  return out;
}

std::string LocalizationService::PrimaryLanguage(std::string_view tag) {
  const auto dash = tag.find('-');
  if (dash == std::string_view::npos) {
    std::string out(tag);
    for (char& ch : out) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
  }
  std::string out(tag.substr(0, dash));
  for (char& ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

} // namespace pbr
