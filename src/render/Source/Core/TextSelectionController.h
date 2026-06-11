#pragma once

#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/Input.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Context;
class Element;
class ElementText;

class TextSelectionController {
public:
	explicit TextSelectionController(Context* context);

	void OnMouseDown(Element* hover, Vector2i mouse_position, int key_modifier_state);
	void OnMouseMove(Vector2i mouse_position);
	void OnMouseUp();
	bool OnKeyDown(Input::KeyIdentifier key, int key_modifier_state);
	void Render();

	/// Pointer down on static selectable content should not steal focus from inputs.
	bool ShouldPreventFocus(Element* hover) const;
	/// Drag-select should not also dispatch a click on mouse up.
	bool ShouldSuppressClick() const;
	/// True while the user is drag-selecting static text.
	bool IsDragging() const;
	/// Nearest clickable ancestor (button, form control, data-event-click).
	Element* FindInteractiveElement(Element* hover) const;

private:
	struct TextSegment {
		ElementText* element = nullptr;
		int flat_begin = 0;
		int flat_end = 0;
	};

	struct LineLayout {
		int begin = 0;
		int length = 0;
		Vector2f baseline;
		float width = 0.f;
		float ascent = 0.f;
		float descent = 0.f;
		ElementText* text_element = nullptr;
	};

	bool IsSelectable(const Element* element) const;
	Element* FindSelectableRoot(Element* hover) const;
	bool IsFormControl(const Element* element) const;
	bool EnsureContainerAlive();
	void RefreshTextFromContainer();
	void CollectTextFromContainer(Element* element, String& out, ElementText*& first_text);
	void ClearSelection();
	void RebuildLayout();
	int HitTest(Vector2f absolute_mouse) const;
	String GetSelectedText() const;
	void BuildSelectionGeometry();

	Context* context;
	Element* container = nullptr;
	ElementText* reference_text = nullptr;
	String flat_text;
	Vector<TextSegment> segments;
	Vector<LineLayout> lines;
	int anchor_index = 0;
	int focus_index = 0;
	bool dragging = false;
	Geometry selection_geometry;
};

} // namespace Rml
