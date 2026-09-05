#include "gui/shell/DocumentLoader.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/AssetIO.h"
#include "gui/shell/RmlMount.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

namespace pbr {

namespace {
Rml::ElementDocument* g_active = nullptr;
}

Rml::ElementDocument* DocumentLoader::LoadFile(Rml::Context* context, const std::string& path) {
  if (!context) {
    return nullptr;
  }
  std::string raw;
  if (!AssetIO::ReadText(path, raw) || raw.empty()) {
    return nullptr;
  }
  const std::string localized = LocalizationService::Instance().LocalizeText(raw);
  // Preserve path as source_url so relative RCSS/asset hrefs resolve like LoadDocument(path).
  return LoadFromMemory(context, localized, path);
}

Rml::ElementDocument* DocumentLoader::LoadFromMemory(Rml::Context* context, const std::string& rml,
                                                   const std::string& source_url) {
  if (!context) {
    return nullptr;
  }
  CloseActive(context);
  auto* document = context->LoadDocumentFromMemory(rml, source_url);
  if (document) {
    document->Show();
    g_active = document;
  }
  return document;
}

bool DocumentLoader::MountFragment(Rml::Element* container, const std::string& rml, MountOptions opts) {
  return RmlMount::MountInner(container, rml, opts);
}

void DocumentLoader::CloseActive(Rml::Context* context) {
  if (g_active) {
    RmlMount::ClearDocumentStyleState(g_active);
    g_active->Close();
    g_active = nullptr;
  }
  (void)context;
}

} // namespace pbr
