#include "ElementCallVideoTile.h"

#include "CallVideoTileRenderer.h"

#include <ui/dom/ElementInstancer.h>
#include <ui/dom/Factory.h>

#include <cstdlib>

namespace pbr {

ElementCallVideoTile::ElementCallVideoTile(const ui::String& tag) : ui::Element(tag) {}

void ElementCallVideoTile::OnRender() {
  const ui::String tile = GetAttribute<ui::String>("tile", "remote");
  if (tile == "local") {
    CallVideoTileRenderer::Instance().RenderTile(CallVideoTileKind::Local, this);
    return;
  }
  if (tile == "peer") {
    const ui::String stream_attr = GetAttribute<ui::String>("stream", "0");
    const uint32_t stream_id = static_cast<uint32_t>(std::strtoul(stream_attr.c_str(), nullptr, 10));
    CallVideoTileRenderer::Instance().RenderTile(CallVideoTileKind::Peer, this, stream_id);
    return;
  }
  CallVideoTileRenderer::Instance().RenderTile(CallVideoTileKind::Remote, this);
}

void RegisterCallVideoTileElement() {
  static ui::ElementInstancerGeneric<ElementCallVideoTile> instancer;
  ui::Factory::RegisterElementInstancer("call-video-tile", &instancer);
}

} // namespace pbr
