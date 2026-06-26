#include "SelectionController.h"

#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "SelectionHighlight.h"
#include "Elements/ElementSelectableText.h"

#include <algorithm>
#include <limits>

namespace Rml {

namespace {

enum class WordCharacterClass { Word, Punctuation, Newline, Whitespace, Undefined };

WordCharacterClass GetWordCharacterClass(char c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || ((unsigned char)c >= 128))
		return WordCharacterClass::Word;
	if ((c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~'))
		return WordCharacterClass::Punctuation;
	if (c == '\n')
		return WordCharacterClass::Newline;
	return WordCharacterClass::Whitespace;
}

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

bool SelectionController::CanSelectStaticText(Element* target) const
{
	if (!target || TargetBlocksSelection(target))
		return false;
	return FindSelectableContainer(target) != nullptr;
}

void SelectionController::SelectWordAt(Vector2i position)
{
	RebuildGlobalMap();
	if (global_text.empty())
		return;

	const int index = HitTestGlobal(Vector2f(float(position.x), float(position.y)));
	if (index < 0 || index >= (int)global_text.size())
		return;

	WordCharacterClass expanding_class = WordCharacterClass::Undefined;
	auto character_is_boundary = [&](int offset) {
		if (offset < 0 || offset >= (int)global_text.size())
			return true;
		const WordCharacterClass character_class = GetWordCharacterClass(global_text[offset]);
		if (expanding_class == WordCharacterClass::Undefined)
		{
			expanding_class = character_class;
			return false;
		}
		return character_class != expanding_class;
	};

	int left = index;
	while (left > 0 && !character_is_boundary(left - 1))
		--left;

	expanding_class = WordCharacterClass::Undefined;
	int right = index;
	while (right < (int)global_text.size() && !character_is_boundary(right))
		++right;

	if (left >= right)
	{
		left = Math::Clamp(index, 0, (int)global_text.size() - 1);
		right = Math::Min(left + 1, (int)global_text.size());
	}

	anchor_index = left;
	focus_index = right;
	dragging = true;
	UpdateSelectionGeometry();
}

void SelectionController::SelectAll()
{
	RebuildGlobalMap();
	if (global_text.empty())
		return;

	anchor_index = 0;
	focus_index = (int)global_text.size();
	dragging = false;
	UpdateSelectionGeometry();
}

void SelectionController::FinalizeSelection()
{
	if (!dragging)
		return;

	RebuildGlobalMap();
	dragging = false;
	UpdateSelectionGeometry();
}

String SelectionController::GetSelectedText()
{
	RebuildGlobalMap();
	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	if (start >= end || start >= (int)global_text.size())
		return {};
	return global_text.substr(start, end - start);
}

String SelectionController::GetSelectedText() const
{
	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	if (start >= end || start >= (int)global_text.size())
		return {};
	return global_text.substr(start, end - start);
}

bool SelectionController::HasSelection() const
{
	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	return start < end;
}

void SelectionController::OnTouchTap(Vector2i /*position*/, Element* hover)
{
	if (!hover || TargetBlocksSelection(hover))
		return;

	if (!IsInsideSelectionRoots(hover) || !HasSelection())
		return;

	ClearSelection();
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
	dragging = false;
	UpdateSelectionGeometry();
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
	handle_drag = SelectionHandleSide::None;
	for (ElementSelectableText* root : roots)
	{
		root->ClearSelectionHighlight();
		root->ClearSelectionHandles();
	}
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
	RebuildGlobalMap();

	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);

	for (ElementSelectableText* root : roots)
	{
		root->ClearSelectionHighlight();
		root->ClearSelectionHandles();
	}

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

	for (const RootBlock& block : blocks)
	{
		if (!block.root)
			continue;

		const bool show_start = (start >= block.global_begin && start <= block.global_end);
		const bool show_end = (end >= block.global_begin && end <= block.global_end);
		if (!show_start && !show_end)
			continue;

		const int local_start_index = show_start ? start - block.global_begin : 0;
		const int local_end_index = show_end ? end - block.global_begin : 0;
		block.root->UpdateSelectionHandles(local_start_index, local_end_index, show_start, show_end);
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

bool SelectionController::ShouldShowHandles() const
{
	if (!HasSelection())
		return false;
	if (handle_drag != SelectionHandleSide::None)
		return true;
	return !dragging;
}

Vector2f SelectionController::GetGlobalIndexPosition(int global_index)
{
	for (const RootBlock& block : blocks)
	{
		if (!block.root)
			continue;
		if (global_index >= block.global_begin && global_index <= block.global_end)
			return block.root->GetAbsolutePositionForFlatIndex(global_index - block.global_begin);
	}
	return {};
}

SelectionHandleSide SelectionController::HitTestHandle(Vector2i position)
{
	if (!ShouldShowHandles())
		return SelectionHandleSide::None;

	RebuildGlobalMap();
	if (global_text.empty() || !context)
		return SelectionHandleSide::None;

	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	if (start >= end)
		return SelectionHandleSide::None;

	const float hit_radius = 28.f * context->GetDensityIndependentPixelRatio();
	const Vector2f point(float(position.x), float(position.y));
	const Vector2f start_pos = GetGlobalIndexPosition(start);
	const Vector2f end_pos = GetGlobalIndexPosition(end);

	if ((point - start_pos).Magnitude() <= hit_radius)
		return SelectionHandleSide::Start;
	if ((point - end_pos).Magnitude() <= hit_radius)
		return SelectionHandleSide::End;
	return SelectionHandleSide::None;
}

bool SelectionController::BeginHandleDrag(SelectionHandleSide side)
{
	if (side == SelectionHandleSide::None)
		return false;

	RebuildGlobalMap();
	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	if (start >= end)
		return false;

	handle_drag = side;
	handle_drag_fixed_index = (side == SelectionHandleSide::Start) ? end : start;
	dragging = false;
	return true;
}

void SelectionController::UpdateHandleDrag(Vector2i position)
{
	if (handle_drag == SelectionHandleSide::None)
		return;

	RebuildGlobalMap();
	const int new_index = HitTestGlobal(Vector2f(float(position.x), float(position.y)));

	if (handle_drag == SelectionHandleSide::Start)
	{
		anchor_index = new_index;
		focus_index = handle_drag_fixed_index;
	}
	else
	{
		anchor_index = handle_drag_fixed_index;
		focus_index = new_index;
	}

	UpdateSelectionGeometry();
}

void SelectionController::EndHandleDrag()
{
	handle_drag = SelectionHandleSide::None;
	dragging = false;
	RebuildGlobalMap();
	UpdateSelectionGeometry();
}

void SelectionController::RenderSelectionHandles()
{
	if (!context || !HasSelection())
		return;

	RebuildGlobalMap();

	RenderManager& render_manager = context->GetRenderManager();
	const RenderState saved_state = render_manager.GetState();
	const float dp_ratio = context->GetDensityIndependentPixelRatio();

	render_manager.SetTransform(nullptr);
	render_manager.DisableClipMask();
	render_manager.SetScissorRegion(Rectanglei::FromSize(render_manager.GetViewport()));

	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);

#if defined(RMLUI_DEBUG_SELECTION_HANDLES)
	RenderSelectionHandleDebugMarker(render_manager, GetGlobalIndexPosition(start), dp_ratio);
	RenderSelectionHandleDebugMarker(render_manager, GetGlobalIndexPosition(end), dp_ratio);
#endif

	if (ShouldShowHandles())
	{
		for (ElementSelectableText* root : roots)
		{
			if (root)
				root->RenderSelectionHandlesAbsolute();
		}
	}

	render_manager.SetState(saved_state);
}

} // namespace Rml
