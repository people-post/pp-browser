#include "ui/DocumentLoader.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

namespace ppbrowser {

namespace {
Rml::ElementDocument* g_active = nullptr;
}

Rml::ElementDocument* DocumentLoader::LoadFile(Rml::Context* context, const std::string& path) {
  if (!context) {
    return nullptr;
  }
  CloseActive(context);
  auto* document = context->LoadDocument(path);
  if (document) {
    document->Show();
    g_active = document;
  }
  return document;
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

void DocumentLoader::CloseActive(Rml::Context* context) {
  if (g_active) {
    g_active->Close();
    g_active = nullptr;
  }
  (void)context;
}

} // namespace ppbrowser
