#pragma once

#include "Header.h"
#include "Types.h"
#include "Vector2.h"

namespace Rml {

class Element;

enum class SelectionDisposition {
	Default,     // defer to children / ancestors
	Participate, // element contributes selectable content or acts as a container
	Block,       // consumes pointer for its own behavior (click, widget)
	Transparent, // invisible to selection; children still considered
};

struct SelectionQuery {
	enum class Phase { PointerDown, PointerMove, PointerUp, Copy };
	Phase phase = Phase::PointerDown;
	Vector2i position;
	Element* target = nullptr;
	bool dragging = false;
};

struct SelectionEndpoint {
	Element* owner = nullptr;
	int index = -1;

	bool IsValid() const { return owner != nullptr && index >= 0; }
};

} // namespace Rml
