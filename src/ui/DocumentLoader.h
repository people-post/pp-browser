#pragma once

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Types.h>
#include <string>

namespace Rml {
class Context;
}

namespace pbr {

class DocumentLoader {
public:
  static Rml::ElementDocument* LoadFile(Rml::Context* context, const std::string& path);
  static Rml::ElementDocument* LoadFromMemory(Rml::Context* context, const std::string& rml,
                                              const std::string& source_url = "[document]");
  static void CloseActive(Rml::Context* context);
};

} // namespace pbr
