#pragma once

#include <string>

namespace ui {
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
  static void ApplyAppearance(ui::Context* context, AppearanceMode preference);
  static void SyncSystemTheme(ui::Context* context);
};

} // namespace pbr
