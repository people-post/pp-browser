#include "SelectionController.h"

#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "Elements/ElementSelectableText.h"

#include <algorithm>
#include <limits>

namespace Rml {

namespace {

void CollectRootsInOrder(Element* element, Vector<ElementSelectableText*>& ordered)
{
	if (!element)
		return;

	if (auto* selectable = rmlui_dynamic_cast<ElementSelectableText*>(element))
	{
		if (selectable->IsSelectionRoot())
			ordered.push_back(selectable);
	}

	for (int i = 0; i < element->GetNumChildren(); ++i)
		CollectRootsInOrder(element->GetChild(i), ordered);
}

} // namespace

SelectionController::SelectionController(Context* context) : context(context) {}

void SelectionController::DiscoverRoots()
{
	roots.clear();
	if (!context)
		return;

	if (Element* root_element = context->GetRootElement())
		CollectRootsInOrder(root_element, roots);
}

void SelectionController::RebuildGlobalMap()
{
	DiscoverRoots();
	global_text.clear();
	blocks.clear();

	for (ElementSelectableText* root : roots)
	{
		if (!root->GetOwnerDocument())
			continue;

		root->RefreshSelectionContent();

		RootBlock block;
		block.root = root;
		block.global_begin = (int)global_text.size();
		global_text += root->GetFlatText();
		block.global_end = (int)global_text.size();
		if (block.global_begin < block.global_end)
			blocks.push_back(block);
	}
}

int SelectionController::HitTestGlobal(Vector2f absolute_mouse) const
{
	if (global_text.empty())
		return 0;

	int best_index = 0;
	float best_distance = std::numeric_limits<float>::max();
	bool found = false;

	for (const RootBlock& block : blocks)
	{
		if (!block.root)
			continue;

		const Vector2f offset = block.root->GetAbsoluteOffset(BoxArea::Border);
		const Vector2f size(block.root->GetOffsetWidth(), block.root->GetOffsetHeight());
		const bool inside = absolute_mouse.x >= offset.x && absolute_mouse.x <= offset.x + size.x && absolute_mouse.y >= offset.y &&
			absolute_mouse.y <= offset.y + size.y;

		const int local_index = block.root->HitTestLocal(absolute_mouse);
		const int global_index = block.global_begin + local_index;

		if (inside)
			return Math::Clamp(global_index, block.global_begin, block.global_end);

		const float center_y = offset.y + size.y * 0.5f;
		const float distance = Math::Absolute(absolute_mouse.y - center_y);
		if (distance < best_distance)
		{
			best_distance = distance;
			if (absolute_mouse.y < center_y)
				best_index = block.global_begin;
			else
				best_index = block.global_end;
			found = true;
		}
	}

	return found ? Math::Clamp(best_index, 0, (int)global_text.size()) : 0;
}

ElementSelectableText* SelectionController::FindSelectableContainer(Element* target) const
{
	for (Element* element = target; element; element = element->GetParentNode())
	{
		if (auto* selectable = rmlui_dynamic_cast<ElementSelectableText*>(element))
			return selectable;
	}
	return nullptr;
}

bool SelectionController::TargetBlocksSelection(Element* target) const
{
	for (Element* element = target; element; element = element->GetParentNode())
	{
		SelectionQuery query;
		query.phase = SelectionQuery::Phase::PointerDown;
		query.target = target;
		if (element->QuerySelection(query) == SelectionDisposition::Block)
			return true;
	}
	return false;
}

bool SelectionController::IsInsideSelectionRoots(Element* element) const
{
	for (Element* current = element; current; current = current->GetParentNode())
	{
		if (auto* selectable = rmlui_dynamic_cast<ElementSelectableText*>(current))
		{
			if (selectable->IsSelectionRoot())
				return true;
		}
	}
	return false;
}

void SelectionController::OnPointerDown(Element* target, Vector2i position)
{
	if (!target || TargetBlocksSelection(target))
		return;

	ElementSelectableText* container = FindSelectableContainer(target);
	if (!container)
		return;

	RebuildGlobalMap();
	if (global_text.empty())
		return;

	const int index = HitTestGlobal(Vector2f(float(position.x), float(position.y)));
	anchor_index = index;
	focus_index = index;
	dragging = true;
	UpdateSelectionGeometry();
}

void SelectionController::OnPointerMove(Vector2i position)
{
	if (!dragging)
		return;

	RebuildGlobalMap();
	focus_index = HitTestGlobal(Vector2f(float(position.x), float(position.y)));
	UpdateSelectionGeometry();
}

void SelectionController::OnPointerUp()
{
	if (!dragging)
		return;

	RebuildGlobalMap();
	UpdateSelectionGeometry();
	dragging = false;
}

bool SelectionController::OnKeyDown(Input::KeyIdentifier key, int key_modifier_state)
{
	const bool ctrl = (key_modifier_state & Input::KM_CTRL) != 0;
	if (!ctrl || key != Input::KI_C)
		return false;

	RebuildGlobalMap();

	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	if (start >= end || start >= (int)global_text.size())
		return false;

	const String selected = global_text.substr(start, end - start);
	if (selected.empty())
		return false;

	if (SystemInterface* system = GetSystemInterface())
		system->SetClipboardText(selected);
	return true;
}

void SelectionController::ClearSelection()
{
	DiscoverRoots();
	anchor_index = 0;
	focus_index = 0;
	dragging = false;
	for (ElementSelectableText* root : roots)
		root->ClearSelectionHighlight();
}

void SelectionController::ClearUnlessHover(Element* hover)
{
	if (hover)
	{
		if (IsInsideSelectionRoots(hover) && !TargetBlocksSelection(hover))
		{
			const int start = Math::Min(anchor_index, focus_index);
			const int end = Math::Max(anchor_index, focus_index);
			if (start < end)
				return;
		}
	}

	DiscoverRoots();
	ClearSelection();
}

void SelectionController::UpdateSelectionGeometry()
{
	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);

	for (ElementSelectableText* root : roots)
		root->ClearSelectionHighlight();

	if (start >= end)
		return;

	for (const RootBlock& block : blocks)
	{
		if (!block.root)
			continue;

		const int local_start = Math::Clamp(start, block.global_begin, block.global_end) - block.global_begin;
		const int local_end = Math::Clamp(end, block.global_begin, block.global_end) - block.global_begin;
		if (local_start < local_end)
			block.root->UpdateSelectionHighlight(local_start, local_end);
	}
}

bool SelectionController::ShouldSuppressClick(Element* target) const
{
	if (!target || TargetBlocksSelection(target))
		return false;

	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	return start < end;
}

} // namespace Rml
