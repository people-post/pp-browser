#pragma once

#include <RmlUi/Core/TextLoupe.h>

class RenderInterface_GL3;

namespace TextLoupeRenderer {

void Render(Rml::TextLoupePhase phase, const Rml::TextLoupeState& state, RenderInterface_GL3& renderer, float dp_ratio);

// Drop capture FBO/programs after an EGL/GL context loss; recreated lazily on next Render.
void ReleaseGpuResources();

} // namespace TextLoupeRenderer
