#include "ClickRouting.h"

#include "../../Include/RmlUi/Core/Element.h"

namespace Rml {
namespace ClickRouting {

namespace {

bool DefaultPointWithin(Element* element, Vector2f point, void* /*context*/)
{
	return element && element->IsPointWithinElement(point);
}

} // namespace

Element* FindInteractiveElement(Element* hover)
{
	for (Element* current = hover; current; current = current->GetParentNode())
	{
		const String& tag = current->GetTagName();
		if (tag == "input" || tag == "textarea" || tag == "select" || tag == "button" || tag == "a")
			return current;
		if (current->HasAttribute("data-event-click"))
			return current;
	}
	return nullptr;
}

bool IsAncestorOf(Element* ancestor, Element* node)
{
	if (!ancestor || !node)
		return false;

	for (Element* walk = node; walk; walk = walk->GetParentNode())
	{
		if (walk == ancestor)
			return true;
	}
	return false;
}

bool InSameClickTree(Element* a, Element* b)
{
	if (!a || !b)
		return false;
	if (a == b)
		return true;
	return IsAncestorOf(a, b) || IsAncestorOf(b, a);
}

Element* ResolveClickTargetWithPredicate(Element* press_hover, Element* release_hover, Vector2f mouse_point, FindFocusElementFn find_focus,
	PointWithinFn point_within, void* point_within_context)
{
	if (!press_hover || !point_within)
		return nullptr;

	auto contains = [&](Element* element) { return point_within(element, mouse_point, point_within_context); };

	// Tier 1: browser default — press and release on the same element or ancestor/descendant chain.
	if (release_hover && InSameClickTree(press_hover, release_hover))
		return release_hover;

	// Tier 2: interactive promotion — layout drift or sibling children under the same control.
	Element* press_interactive = FindInteractiveElement(press_hover);
	if (press_interactive)
	{
		Element* release_interactive = release_hover ? FindInteractiveElement(release_hover) : nullptr;
		if (release_interactive == press_interactive && contains(press_interactive))
			return press_interactive;
		return nullptr;
	}

	// Tier 3: non-interactive press — focus match or geometry on the press target.
	if (find_focus && release_hover && press_hover == find_focus(release_hover))
		return press_hover;

	if (contains(press_hover) && (!release_hover || InSameClickTree(press_hover, release_hover)))
		return press_hover;

	return nullptr;
}

Element* ResolveClickTarget(Element* press_hover, Element* release_hover, Vector2f mouse_point, FindFocusElementFn find_focus)
{
	return ResolveClickTargetWithPredicate(press_hover, release_hover, mouse_point, find_focus, DefaultPointWithin, nullptr);
}

} // namespace ClickRouting
} // namespace Rml
