#include "foundation/platform/os/OsLocale.h"

#import <Foundation/Foundation.h>

#include <string>
#include <vector>

namespace pbr::os {

// Apple apps launched from the home screen / Finder carry no LANG / LC_* variables, so the POSIX
// reader always came back empty and the UI fell back to English. Ask the OS instead: the user's
// ordered language list from Settings → General → Language & Region (BCP-47, e.g. "zh-Hans-CN").
std::vector<std::string> PreferredSystemLocales() {
  std::vector<std::string> out;
  @autoreleasepool {
    for (NSString* tag in [NSLocale preferredLanguages]) {
      if (const char* utf8 = [tag UTF8String]; utf8 != nullptr && utf8[0] != '\0') {
        out.emplace_back(utf8);
      }
    }
  }
  return out;
}

} // namespace pbr::os
