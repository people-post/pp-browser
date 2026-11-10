#include "gui/shell/DeferredStartup.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/IAssetLocator.h"
#include "common/Logger.h"
#include "common/StartupTiming.h"
#include "foundation/crypto/ProfileUnlockGate.h"
#include "gui/ClientCompatController.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <ui/Core.h>
#include <ui/base/FileInterface.h>

#include <cstddef>
#include <map>
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

/**
 * Simplified Chinese in several weights, one fallback family so the engine picks the weight nearest
 * to the text's: face kCjkFaceSc of the collection (400) plus these (notofonts/noto-cjk Sans/SubsetOTF/SC).
 */
constexpr const char* kChineseFamily = "Noto Sans CJK SC";
constexpr const char* kChineseMediumAsset = "fonts/NotoSansSC-Medium.otf";
constexpr const char* kChineseBoldAsset = "fonts/NotoSansSC-Bold.otf";

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
 * Process-lifetime buffer for a packaged font file (the shared CJK collection, the Chinese weights).
 * RmlUi's memory LoadFontFace does not copy these bytes — they must outlive font faces
 * (valid until after ui::Shutdown). Empty when the file could not be read.
 */
const std::vector<ui::byte>& FontAssetBytes(const char* relative) {
  static std::map<std::string, std::vector<ui::byte>> buffers;
  auto [it, added] = buffers.try_emplace(relative);
  auto& bytes = it->second;
  if (!added) {
    return bytes;
  }

  const std::string path = IAssetLocator::Instance().Resolve(relative);
  ui::FileInterface* files = ui::GetFileInterface();
  if (files == nullptr) {
    log().warning << "Deferred font load failed: no FileInterface (" << relative << ")";
    return bytes;
  }

  const ui::FileHandle handle = files->Open(path);
  if (!handle) {
    log().warning << "Deferred font load failed: open " << path;
    return bytes;
  }

  const size_t length = files->Length(handle);
  if (length == 0) {
    files->Close(handle);
    log().warning << "Deferred font load failed: empty " << path;
    return bytes;
  }

  bytes.resize(length);
  const size_t read = files->Read(bytes.data(), length, handle);
  files->Close(handle);
  if (read != length) {
    bytes.clear();
    log().warning << "Deferred font load failed: short read " << path;
    return bytes;
  }

  log().info << "Loaded font file once (" << (length / (1024 * 1024)) << " MiB): " << relative;
  return bytes;
}

bool LoadFallbackFaceFromMemory(const std::vector<ui::byte>& bytes, const std::string& family, int weight,
                                int face_index) {
  return !bytes.empty() &&
         ui::LoadFontFace(ui::Span<const ui::byte>(bytes.data(), bytes.size()), family, ui::Style::FontStyle::Normal,
                          static_cast<ui::Style::FontWeight>(weight), true /*fallback_face*/, face_index);
}

void LoadCjkFaceFromCollection(int face_index, const char* phase_name) {
  StartupPhase phase(phase_name);
  const auto& bytes = FontAssetBytes(kCjkCollectionAsset);
  if (bytes.empty()) {
    return;
  }
  // Empty family: FreeType fills family/style from the selected collection face.
  const bool ok =
      ui::LoadFontFace(ui::Span<const ui::byte>(bytes.data(), bytes.size()), "", ui::Style::FontStyle::Normal,
                       ui::Style::FontWeight::Auto, true /*fallback_face*/, face_index);
  if (!ok) {
    log().warning << "Deferred CJK face load failed: index=" << face_index;
  }
}

} // namespace

bool LoadChineseFonts() {
  const bool regular = LoadFallbackFaceFromMemory(FontAssetBytes(kCjkCollectionAsset), kChineseFamily, 400, kCjkFaceSc);
  const bool medium = LoadFallbackFaceFromMemory(FontAssetBytes(kChineseMediumAsset), kChineseFamily, 500, 0);
  const bool bold = LoadFallbackFaceFromMemory(FontAssetBytes(kChineseBoldAsset), kChineseFamily, 700, 0);
  if (!medium || !bold) {
    log().warning << "Chinese medium / bold faces failed to load; Chinese in heavier text stays regular";
  }
  if (!regular) {
    log().warning << "Deferred CJK face load failed: index=" << kCjkFaceSc;
  }
  return regular;
}

namespace {

void LoadFallbackFace(const std::string& relative, const char* phase_name) {
  StartupPhase phase(phase_name);
  const bool ok = ui::LoadFontFace(IAssetLocator::Instance().Resolve(relative), true);
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
    if (cjk_faces[i].first == kCjkFaceSc) {
      StartupPhase sc_phase(cjk_faces[i].second);
      LoadChineseFonts();
    } else {
      LoadCjkFaceFromCollection(cjk_faces[i].first, cjk_faces[i].second);
    }
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
