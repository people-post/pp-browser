#include "gui/shell/DeferredStartup.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/IAssetLocator.h"
#include "common/Logger.h"
#include "common/StartupTiming.h"
#include "foundation/crypto/ProfileUnlockGate.h"
#include "gui/ClientCompatController.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

auto& log() {
  static auto logger = logging::getLogger("DeferredStartup");
  return logger;
}

bool g_started = false;

/** Noto Sans CJK Regular OTC face indexes (notofonts/noto-cjk Sans/OTC). */
constexpr const char* kCjkCollectionAsset = "fonts/NotoSansCJK-Regular.ttc";
constexpr int kCjkFaceJp = 0;
constexpr int kCjkFaceKr = 1;
constexpr int kCjkFaceSc = 2;
constexpr int kCjkFaceTc = 3;
constexpr int kCjkFaceHk = 4;

std::string PrimaryLang(const std::string& tag) {
  const auto dash = tag.find('-');
  return dash == std::string::npos ? tag : tag.substr(0, dash);
}

/**
 * Preferred CJK face index for the current UI language.
 * Returns -1 when chrome does not need a CJK face.
 */
int PrimaryCjkFaceIndex() {
  const std::string& tag = LocalizationService::Instance().ResolvedLanguage();
  const std::string primary = PrimaryLang(tag);
  if (primary == "zh") {
    if (tag.find("HK") != std::string::npos) {
      return kCjkFaceHk;
    }
    if (tag.find("Hant") != std::string::npos || tag.find("TW") != std::string::npos) {
      return kCjkFaceTc;
    }
    return kCjkFaceSc;
  }
  if (primary == "ja") {
    return kCjkFaceJp;
  }
  if (primary == "ko") {
    return kCjkFaceKr;
  }
  return -1;
}

/**
 * Process-lifetime buffer for the shared CJK collection. RmlUi's memory
 * LoadFontFace does not copy these bytes — they must outlive font faces
 * (valid until after Rml::Shutdown).
 */
std::vector<Rml::byte>& CjkCollectionBytes() {
  static std::vector<Rml::byte> bytes;
  return bytes;
}

bool EnsureCjkCollectionLoaded() {
  auto& bytes = CjkCollectionBytes();
  if (!bytes.empty()) {
    return true;
  }

  const std::string path = IAssetLocator::Instance().Resolve(kCjkCollectionAsset);
  Rml::FileInterface* files = Rml::GetFileInterface();
  if (files == nullptr) {
    log().warning << "Deferred CJK load failed: no FileInterface (" << kCjkCollectionAsset << ")";
    return false;
  }

  const Rml::FileHandle handle = files->Open(path);
  if (!handle) {
    log().warning << "Deferred CJK load failed: open " << path;
    return false;
  }

  const size_t length = files->Length(handle);
  if (length == 0) {
    files->Close(handle);
    log().warning << "Deferred CJK load failed: empty " << path;
    return false;
  }

  bytes.resize(length);
  const size_t read = files->Read(bytes.data(), length, handle);
  files->Close(handle);
  if (read != length) {
    bytes.clear();
    log().warning << "Deferred CJK load failed: short read " << path;
    return false;
  }

  log().info << "Loaded CJK font collection once (" << (length / (1024 * 1024)) << " MiB): "
             << kCjkCollectionAsset;
  return true;
}

void LoadCjkFaceFromCollection(int face_index, const char* phase_name) {
  StartupPhase phase(phase_name);
  if (!EnsureCjkCollectionLoaded()) {
    return;
  }
  auto& bytes = CjkCollectionBytes();
  // Empty family: FreeType fills family/style from the selected collection face.
  const bool ok =
      Rml::LoadFontFace(Rml::Span<const Rml::byte>(bytes.data(), bytes.size()), "", Rml::Style::FontStyle::Normal,
                        Rml::Style::FontWeight::Auto, true /*fallback_face*/, face_index);
  if (!ok) {
    log().warning << "Deferred CJK face load failed: index=" << face_index;
  }
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
  const int primary = PrimaryCjkFaceIndex();

  // Regional faces only (indexes 0–4). All share one OTC buffer in memory.
  std::vector<std::pair<int, const char*>> cjk_faces;
  if (need_cjk_for_ui && primary >= 0) {
    cjk_faces.emplace_back(primary, "LoadFontFace:primary_cjk");
  }
  const int all_cjk[] = {kCjkFaceSc, kCjkFaceJp, kCjkFaceKr, kCjkFaceTc, kCjkFaceHk};
  for (int face_index : all_cjk) {
    if (primary >= 0 && face_index == primary) {
      continue;
    }
    cjk_faces.emplace_back(face_index, "LoadFontFace:deferred_cjk");
  }

  for (size_t i = 0; i < cjk_faces.size(); ++i) {
    LoadCjkFaceFromCollection(cjk_faces[i].first, cjk_faces[i].second);
    if (need_cjk_for_ui && i == 0) {
      MarkFontsReadyAndRefresh(shell);
    }
  }

  // Color emoji so CBDT glyphs win over monochrome outlines.
  LoadFallbackFace("fonts/NotoColorEmoji.ttf", "LoadFontFace:NotoColorEmoji");
  LoadFallbackFace("fonts/NotoEmoji-Regular.ttf", "LoadFontFace:NotoEmoji");

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
  // Drop the first-paint arm: cover stays only while unlock_in_progress (PIN wins).
  if (shell.settle_startup_cover) {
    shell.settle_startup_cover();
  }
  client_compat.CheckAsync();
  LoadDeferredFonts(shell);
}

} // namespace pbr
