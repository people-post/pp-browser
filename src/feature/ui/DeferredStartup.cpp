#include "feature/ui/DeferredStartup.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/IAssetLocator.h"
#include "common/Logger.h"
#include "common/StartupTiming.h"
#include "foundation/crypto/ProfileUnlockGate.h"
#include "feature/ui/ClientCompatController.h"
#include "feature/ui/ShellNavigationPorts.h"

#include <RmlUi/Core/Core.h>

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

auto& log() {
  static auto logger = logging::getLogger("DeferredStartup");
  return logger;
}

bool g_started = false;

std::string PrimaryLang(const std::string& tag) {
  const auto dash = tag.find('-');
  return dash == std::string::npos ? tag : tag.substr(0, dash);
}

/** Preferred CJK face for the current UI language (empty if none required for chrome). */
std::string PrimaryCjkFontRelative() {
  const std::string& tag = LocalizationService::Instance().ResolvedLanguage();
  const std::string primary = PrimaryLang(tag);
  if (primary == "zh") {
    if (tag.find("Hant") != std::string::npos || tag.find("TW") != std::string::npos ||
        tag.find("HK") != std::string::npos) {
      return "fonts/NotoSansCJKtc-Regular.otf";
    }
    return "fonts/NotoSansCJKsc-Regular.otf";
  }
  if (primary == "ja") {
    return "fonts/NotoSansCJKjp-Regular.otf";
  }
  if (primary == "ko") {
    return "fonts/NotoSansCJKkr-Regular.otf";
  }
  return {};
}

void LoadFallbackFace(const std::string& relative, const char* phase_name) {
  StartupPhase phase(phase_name);
  const bool ok = Rml::LoadFontFace(IAssetLocator::Instance().Resolve(relative), true);
  if (!ok) {
    log().warning << "Deferred font load failed: " << relative;
  }
}

void MarkFontsReadyAndRefresh(const ShellNavigationPorts& shell) {
  if (!shell.set_fonts_ready) {
    return;
  }
  if (shell.fonts_ready && shell.fonts_ready()) {
    return;
  }
  shell.set_fonts_ready(true);
  // SyncLayout remounts + DirtyWindow (includes fonts_ready); no separate dirty.
  if (shell.request_sync_layout) {
    shell.request_sync_layout(true, "deferred_fonts_ready");
  }
  StartupMark("fonts_ready");
}

void LoadDeferredFonts(const ShellNavigationPorts& shell) {
  StartupPhase phase("DeferredStartup::LoadFonts");
  const bool need_cjk_for_ui = UiLanguageNeedsCjkFonts();
  const std::string primary = PrimaryCjkFontRelative();

  std::vector<std::pair<std::string, const char*>> faces;
  if (need_cjk_for_ui && !primary.empty()) {
    faces.emplace_back(primary, "LoadFontFace:primary_cjk");
  }
  const char* all_cjk[] = {
      "fonts/NotoSansCJKsc-Regular.otf",
      "fonts/NotoSansCJKjp-Regular.otf",
      "fonts/NotoSansCJKkr-Regular.otf",
      "fonts/NotoSansCJKtc-Regular.otf",
  };
  for (const char* path : all_cjk) {
    if (!primary.empty() && primary == path) {
      continue;
    }
    faces.emplace_back(path, "LoadFontFace:deferred_cjk");
  }
  // Color emoji first so CBDT glyphs win over monochrome outlines.
  faces.emplace_back("fonts/NotoColorEmoji.ttf", "LoadFontFace:NotoColorEmoji");
  faces.emplace_back("fonts/NotoEmoji-Regular.ttf", "LoadFontFace:NotoEmoji");

  for (size_t i = 0; i < faces.size(); ++i) {
    LoadFallbackFace(faces[i].first, faces[i].second);
    if (need_cjk_for_ui && i == 0) {
      MarkFontsReadyAndRefresh(shell);
    }
  }

  if (!need_cjk_for_ui) {
    StartupMark("deferred_fonts_complete");
  } else if (!shell.fonts_ready || !shell.fonts_ready()) {
    MarkFontsReadyAndRefresh(shell);
  } else {
    StartupMark("deferred_fonts_complete");
  }
}

} // namespace

bool UiLanguageNeedsCjkFonts() {
  const std::string primary = PrimaryLang(LocalizationService::Instance().ResolvedLanguage());
  return primary == "zh" || primary == "ja" || primary == "ko";
}

void OnFirstPresentDeferredStartup(ClientCompatController& client_compat, ProfileUnlockGate& unlock_gate,
                                   const ShellNavigationPorts& shell) {
  if (g_started) {
    return;
  }
  g_started = true;
  StartupMark("deferred_startup_begin");

  // Kick vault unlock first (runs off-UI via ProfileUnlockPorts::run_heavy). Loading CJK
  // fallbacks on the UI thread used to serialize behind Argon2 and freeze first paint.
  unlock_gate.BeginDeferredUnlockAfterFirstPresent();
  client_compat.CheckAsync();
  LoadDeferredFonts(shell);
}

} // namespace pbr
