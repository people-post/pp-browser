#pragma once

#include <RmlUi/Core/TextLoupe.h>

class RenderInterface_GL3;

namespace TextLoupeRenderer {

void Render(Rml::TextLoupePhase phase, const Rml::TextLoupeState& state, RenderInterface_GL3& renderer, float dp_ratio);

} // namespace TextLoupeRenderer
