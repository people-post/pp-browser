#pragma once

#include "Header.h"
#include "Types.h"
#include "Vector2.h"

#include <functional>

namespace Rml {

class RenderManager;

struct TextLoupeState {
	bool active = false;
	Vector2f anchor;
};

enum class TextLoupePhase { Capture, Draw };

using TextLoupeRenderCallback = std::function<void(TextLoupePhase phase, const TextLoupeState& state, RenderManager& render_manager)>;

} // namespace Rml
