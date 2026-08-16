#include "ElementCallVideoTile.h"

#include "CallVideoTileRenderer.h"

#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>

namespace pbr {

ElementCallVideoTile::ElementCallVideoTile(const Rml::String& tag) : Rml::Element(tag) {}

void ElementCallVideoTile::OnRender() {
  const bool local = GetAttribute<Rml::String>("tile", "remote") == "local";
  CallVideoTileRenderer::Instance().RenderTile(
      local ? CallVideoTileKind::Local : CallVideoTileKind::Remote, this);
}

void RegisterCallVideoTileElement() {
  static Rml::ElementInstancerGeneric<ElementCallVideoTile> instancer;
  Rml::Factory::RegisterElementInstancer("call-video-tile", &instancer);
}

} // namespace pbr
