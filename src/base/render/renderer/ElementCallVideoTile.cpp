#include "ElementCallVideoTile.h"

#include "CallVideoTileRenderer.h"

#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>

#include <cstdlib>

namespace pbr {

ElementCallVideoTile::ElementCallVideoTile(const Rml::String& tag) : Rml::Element(tag) {}

void ElementCallVideoTile::OnRender() {
  const Rml::String tile = GetAttribute<Rml::String>("tile", "remote");
  if (tile == "local") {
    CallVideoTileRenderer::Instance().RenderTile(CallVideoTileKind::Local, this);
    return;
  }
  if (tile == "peer") {
    const Rml::String stream_attr = GetAttribute<Rml::String>("stream", "0");
    const uint32_t stream_id = static_cast<uint32_t>(std::strtoul(stream_attr.c_str(), nullptr, 10));
    CallVideoTileRenderer::Instance().RenderTile(CallVideoTileKind::Peer, this, stream_id);
    return;
  }
  CallVideoTileRenderer::Instance().RenderTile(CallVideoTileKind::Remote, this);
}

void RegisterCallVideoTileElement() {
  static Rml::ElementInstancerGeneric<ElementCallVideoTile> instancer;
  Rml::Factory::RegisterElementInstancer("call-video-tile", &instancer);
}

} // namespace pbr
