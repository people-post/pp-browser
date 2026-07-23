#pragma once

#include "common/Error.h"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pbr {

struct LocaleInfo {
  std::string tag;
  /** Catalog key for native self-name (e.g. settings.language.zh_hans). */
  std::string native_name_key;
};

class LocalizationService {
public:
  static LocalizationService& Instance();

  /** Load catalogs from `{assets_root}/locales` (all `.json` files). */
  Roe<void> LoadFromAssets(const std::string& assets_root);

  /** Preferred pref: `system` or a BCP-47 tag we ship. */
  void SetPreferredLanguage(std::string pref);
  std::string PreferredLanguage() const { return preferred_; }
  std::string ResolvedLanguage() const { return resolved_; }

  /** Override OS locales for tests (empty = use SDL). */
  void SetSystemLocalesForTest(std::vector<std::string> locales);
  void ClearSystemLocalesForTest();

  std::string Tr(std::string_view key) const;
  std::string Tr(std::string_view key, const std::map<std::string, std::string>& args) const;

  std::vector<LocaleInfo> AvailableLocales() const;

  /** Replace `{{i18n:key}}` tokens in RML/text. Leaves unknown tokens unchanged. */
  std::string LocalizeText(std::string_view input) const;

  void AddLanguageChangeListener(std::function<void(const std::string& resolved)> listener);

  /** Display label for a language pref (`system` → translated System; else native name). */
  std::string LanguageDisplayLabel(std::string_view pref) const;

private:
  LocalizationService() = default;

  void ResolveAndNotify(bool notify);
  std::string ResolvePreferred(const std::string& pref) const;
  std::vector<std::string> PreferredSystemLocales() const;
  std::string Lookup(std::string_view key) const;
  static std::string Interpolate(std::string templ, const std::map<std::string, std::string>& args);
  static std::string NormalizeTag(std::string_view tag);
  static std::string PrimaryLanguage(std::string_view tag);

  std::string preferred_ = "system";
  std::string resolved_ = "en";
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> catalogs_;
  std::vector<LocaleInfo> available_;
  std::vector<std::function<void(const std::string&)>> listeners_;
  std::vector<std::string> system_locales_override_;
  bool use_system_locales_override_ = false;
};

/** Shortcut to LocalizationService::Instance().Tr(key). */
inline std::string Tr(std::string_view key) {
  return LocalizationService::Instance().Tr(key);
}

inline std::string Tr(std::string_view key, const std::map<std::string, std::string>& args) {
  return LocalizationService::Instance().Tr(key, args);
}

} // namespace pbr
