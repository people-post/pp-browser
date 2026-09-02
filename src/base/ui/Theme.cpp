#include "base/ui/Theme.h"

#include "foundation/platform/AndroidSystemChrome.h"
#include "foundation/platform/AssetIO.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <SDL3/SDL.h>

namespace pbr {

namespace {

AppearanceMode g_active_preference = AppearanceMode::System;
Rml::Context* g_context = nullptr;

bool SystemPrefersDark() {
  switch (SDL_GetSystemTheme()) {
  case SDL_SYSTEM_THEME_DARK:
    return true;
  case SDL_SYSTEM_THEME_LIGHT:
    return false;
  default:
    return false;
  }
}

void ActivateResolvedTheme(Rml::Context* context, bool dark) {
  if (!context) {
    return;
  }
  context->ActivateTheme("light", !dark);
  context->ActivateTheme("dark", dark);
  for (int i = 0; i < context->GetNumDocuments(); ++i) {
    if (auto* document = context->GetDocument(i)) {
      document->UpdateDocument();
    }
  }
}

} // namespace

bool Theme::LoadBase(const std::string& rcss_path) {
  return AssetIO::Exists(rcss_path);
}

AppearanceMode Theme::ParseAppearance(const std::string& value) {
  if (value == "light") {
    return AppearanceMode::Light;
  }
  if (value == "dark") {
    return AppearanceMode::Dark;
  }
  return AppearanceMode::System;
}

std::string Theme::ToAppearanceString(AppearanceMode mode) {
  switch (mode) {
  case AppearanceMode::Light:
    return "light";
  case AppearanceMode::Dark:
    return "dark";
  case AppearanceMode::System:
  default:
    return "system";
  }
}

bool Theme::ResolveDark(AppearanceMode preference) {
  switch (preference) {
  case AppearanceMode::Light:
    return false;
  case AppearanceMode::Dark:
    return true;
  case AppearanceMode::System:
  default:
    return SystemPrefersDark();
  }
}

void Theme::ApplyAppearance(Rml::Context* context, AppearanceMode preference) {
  g_active_preference = preference;
  g_context = context;
  ActivateResolvedTheme(context, ResolveDark(preference));
  AndroidSystemChrome::SetAppearance(ToAppearanceString(preference));
}

void Theme::SyncSystemTheme(Rml::Context* context) {
  if (g_active_preference != AppearanceMode::System) {
    return;
  }
  ApplyAppearance(context, AppearanceMode::System);
}

} // namespace pbr
