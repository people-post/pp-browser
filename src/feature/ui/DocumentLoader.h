#pragma once

#include "feature/ui/RmlMount.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Types.h>
#include <string>

namespace Rml {
class Context;
class Element;
}

namespace pbr {

class DocumentLoader {
public:
  static Rml::ElementDocument* LoadFile(Rml::Context* context, const std::string& path);
  static Rml::ElementDocument* LoadFromMemory(Rml::Context* context, const std::string& rml,
                                              const std::string& source_url = "[document]");
  static bool MountFragment(Rml::Element* container, const std::string& rml, MountOptions opts = {});
  static void CloseActive(Rml::Context* context);
};

} // namespace pbr
