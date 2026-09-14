#pragma once

#include "gui/shell/RmlMount.h"

#include <ui/dom/ElementDocument.h>
#include <ui/base/Types.h>
#include <string>

namespace ui {
class Context;
class Element;
}

namespace pbr {

class DocumentLoader {
public:
  static ui::ElementDocument* LoadFile(ui::Context* context, const std::string& path);
  static ui::ElementDocument* LoadFromMemory(ui::Context* context, const std::string& rml,
                                              const std::string& source_url = "[document]");
  static bool MountFragment(ui::Element* container, const std::string& rml, MountOptions opts = {});
  static void CloseActive(ui::Context* context);
};

} // namespace pbr
