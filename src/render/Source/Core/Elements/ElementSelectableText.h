#pragma once

#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/EventListener.h"
#include "../../../Include/RmlUi/Core/Geometry.h"
#include "../../../Include/RmlUi/Core/Input.h"

namespace Rml {

class ElementText;

/// Static text container with drag-selection and Ctrl+C copy. Created for elements with selectable="text".
class ElementSelectableText : public Element, public EventListener {
public:
	RMLUI_RTTI_DefineWithParent(ElementSelectableText, Element)

	explicit ElementSelectableText(const String& tag);
	~ElementSelectableText() override;

	void OnRender() override;

	/// Extends drag-select when the pointer moves outside this element (called from Context).
	static void NotifyGlobalMouseMove(Vector2i mouse_position);
	/// Ctrl+C copy when focus is elsewhere (called from Context before focus keydown).
	static bool NotifyGlobalKeyDown(Input::KeyIdentifier key, int key_modifier_state);
	/// True while any selectable region is drag-selecting (blocks RmlUi element drag).
	static bool IsAnyDragging();
	/// Clear persisted selections when the pointer is outside selectable static text.
	static void ClearSelectionsUnlessContaining(Element* hover);

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

	void ProcessEvent(Event& event) override;

	static bool IsInteractiveElement(const Element* element);
	void ClearSelection();
	void RefreshTextFromContainer();
	void CollectTextFromContainer(Element* element, String& out, ElementText*& first_text);
	void RebuildLayout();
	int HitTest(Vector2f absolute_mouse) const;
	String GetSelectedText() const;
	void BuildSelectionGeometry();
	bool HasNonEmptySelection() const;
	void BeginSelection(Vector2i mouse_position);
	void ExtendSelection(Vector2i mouse_position);
	void EndSelection();
	void RegisterInstance();
	void UnregisterInstance();

	static ElementSelectableText* active_dragger;

	Vector<TextSegment> segments;
	Vector<LineLayout> lines;
	String flat_text;
	ElementText* reference_text = nullptr;
	int anchor_index = 0;
	int focus_index = 0;
	bool dragging = false;
	bool suppress_click = false;
	Geometry selection_geometry;
};

} // namespace Rml
