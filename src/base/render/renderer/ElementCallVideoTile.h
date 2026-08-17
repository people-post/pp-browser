#pragma once

#include <RmlUi/Core/Element.h>

namespace pbr {

/**
 * Shell call video tile — paints persistent GL textures from CallVideoTileRenderer
 * during Element::OnRender so stacking matches in-call chrome (V018 path B).
 *
 * RML: <call-video-tile tile="remote|local" ...>
 */
class ElementCallVideoTile : public Rml::Element {
public:
  RMLUI_RTTI_DefineWithParent(ElementCallVideoTile, Rml::Element)

  explicit ElementCallVideoTile(const Rml::String& tag);

protected:
  void OnRender() override;
};

/** Register the `call-video-tile` instancer (call after Rml::Initialise). */
void RegisterCallVideoTileElement();

} // namespace pbr
