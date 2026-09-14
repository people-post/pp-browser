#pragma once

#include <ui/dom/Element.h>

namespace pbr {

/**
 * Shell call video tile — paints persistent GL textures from CallVideoTileRenderer
 * during Element::OnRender so stacking matches in-call chrome (V018 path B).
 *
 * RML: <call-video-tile tile="remote|local|peer" stream="123" ...>
 */
class ElementCallVideoTile : public ui::Element {
public:
  UI_RTTI_DefineWithParent(ElementCallVideoTile, ui::Element)

  explicit ElementCallVideoTile(const ui::String& tag);

protected:
  void OnRender() override;
};

/** Register the `call-video-tile` instancer (call after ui::Initialise). */
void RegisterCallVideoTileElement();

} // namespace pbr
