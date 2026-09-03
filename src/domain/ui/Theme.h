#pragma once

#include <string>

namespace Rml {
class Context;
}

namespace pbr {

enum class AppearanceMode { System, Light, Dark };

class Theme {
public:
  static bool LoadBase(const std::string& rcss_path);

  static AppearanceMode ParseAppearance(const std::string& value);
  static std::string ToAppearanceString(AppearanceMode mode);

  static bool ResolveDark(AppearanceMode preference);
  static void ApplyAppearance(Rml::Context* context, AppearanceMode preference);
  static void SyncSystemTheme(Rml::Context* context);
};

} // namespace pbr
