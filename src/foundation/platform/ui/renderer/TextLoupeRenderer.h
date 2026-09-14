#pragma once

#include <ui/base/TextLoupe.h>

class RenderInterface_GL3;

namespace TextLoupeRenderer {

void Render(ui::TextLoupePhase phase, const ui::TextLoupeState& state, RenderInterface_GL3& renderer, float dp_ratio);

// Drop capture FBO/programs after an EGL/GL context loss; recreated lazily on next Render.
void ReleaseGpuResources();

} // namespace TextLoupeRenderer
