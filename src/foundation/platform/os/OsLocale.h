#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pbr::os {

/** OS-preferred UI locales (BCP-47-ish tags). Empty if unknown. */
std::vector<std::string> PreferredSystemLocales();

/** "zh-Hans-CN, en-US" → {"zh-Hans-CN", "en-US"}: trims, drops empties (Android's LocaleList.toLanguageTags). */
std::vector<std::string> SplitLanguageTags(std::string_view csv);

} // namespace pbr::os
