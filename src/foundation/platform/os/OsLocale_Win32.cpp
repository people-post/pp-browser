#include "foundation/platform/os/OsLocale.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pbr::os {

std::vector<std::string> PreferredSystemLocales() {
  std::vector<std::string> out;
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) <= 0) {
    return out;
  }
  char narrow[LOCALE_NAME_MAX_LENGTH] = {};
  if (WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr) <= 0) {
    return out;
  }
  if (narrow[0] != '\0') {
    out.emplace_back(narrow);
  }
  return out;
}

} // namespace pbr::os

#endif // _WIN32
