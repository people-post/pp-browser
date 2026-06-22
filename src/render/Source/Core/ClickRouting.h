#pragma once

#include "../../Include/RmlUi/Core/Header.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;

/// Pointer-driven click target resolution for Rml::Context.
namespace ClickRouting {

using FindFocusElementFn = Element* (*)(Element*);
using PointWithinFn = bool (*)(Element*, Vector2f, void*);

/// Nearest clickable ancestor (button, form control, data-event-click).
Element* FindInteractiveElement(Element* hover);

/// True when @p ancestor is on the parent chain of @p node.
bool IsAncestorOf(Element* ancestor, Element* node);

/// True when @p a and @p b are the same element or one is an ancestor of the other.
bool InSameClickTree(Element* a, Element* b);

/// Resolve the element that should receive a synthesized click on mouseup.
Element* ResolveClickTarget(Element* press_hover, Element* release_hover, Vector2f mouse_point, FindFocusElementFn find_focus);

/// Same as ResolveClickTarget with a custom point-in-element predicate (used by unit tests).
Element* ResolveClickTargetWithPredicate(Element* press_hover, Element* release_hover, Vector2f mouse_point, FindFocusElementFn find_focus,
	PointWithinFn point_within, void* point_within_context);

} // namespace ClickRouting
} // namespace Rml
