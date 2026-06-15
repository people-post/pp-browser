#pragma once

#include "../../Include/RmlUi/Core/Input.h"
#include "../../Include/RmlUi/Core/SelectionTypes.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "../../Include/RmlUi/Core/Vector2.h"

namespace Rml {

class Context;
class Element;
class ElementSelectableText;

class SelectionController {
public:
	explicit SelectionController(Context* context);

	void OnPointerDown(Element* target, Vector2i position);
	void OnPointerMove(Vector2i position);
	void OnPointerUp();
	bool OnKeyDown(Input::KeyIdentifier key, int key_modifier_state);
	void ClearUnlessHover(Element* hover);
	bool IsDragging() const { return dragging; }

	void UpdateSelectionGeometry();
	void ClearSelection();

	bool ShouldSuppressClick(Element* target) const;

private:
	struct RootBlock {
		ElementSelectableText* root = nullptr;
		int global_begin = 0;
		int global_end = 0;
	};

	Context* context;
	Vector<ElementSelectableText*> roots;
	Vector<RootBlock> blocks;
	String global_text;
	int anchor_index = 0;
	int focus_index = 0;
	bool dragging = false;

	void DiscoverRoots();
	void RebuildGlobalMap();
	int HitTestGlobal(Vector2f absolute_mouse) const;
	ElementSelectableText* FindSelectableContainer(Element* target) const;
	bool TargetBlocksSelection(Element* target) const;
	bool IsInsideSelectionRoots(Element* element) const;
};

} // namespace Rml
