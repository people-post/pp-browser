#pragma once

#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/EventListener.h"
#include "../../../Include/RmlUi/Core/Geometry.h"
#include "../../../Include/RmlUi/Core/Input.h"
#include "../../../Include/RmlUi/Core/SelectionTypes.h"
#include "ElementTextSelection.h"

namespace Rml {

class ElementText;
class SelectionContentBuilder;

/// Static text container with drag-selection and Ctrl+C copy. Created for elements with selectable="text".
class ElementSelectableText : public Element, public EventListener, public SelectionStyleClient {
public:
	RMLUI_RTTI_DefineWithParent(ElementSelectableText, Element)

	explicit ElementSelectableText(const String& tag);
	~ElementSelectableText() override;

	SelectionDisposition QuerySelection(const SelectionQuery& query) override;
	void BuildSelectionContent(SelectionContentBuilder& builder) override;
	SelectionEndpoint HitTestSelection(Vector2f absolute_position) const override;
	String GetSelectionSlice(int local_start, int local_end) const override;

	bool IsSelectionRoot() const;
	void RefreshSelectionContent();
	const String& GetFlatText() const { return flat_text; }
	int HitTestLocal(Vector2f absolute_mouse);
	void UpdateSelectionHighlight(int local_start, int local_end);
	void ClearSelectionHighlight();
	void UpdateSelectionHandles(int local_start_index, int local_end_index, bool show_start, bool show_end);
	void ClearSelectionHandles();
	Vector2f GetAbsolutePositionForFlatIndex(int flat_index);

	void OnRender() override;

	void OnSelectionStyleChanged() override;

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

	void RebuildLayout();
	Vector2f GetContentRenderOrigin();
	void RebuildActiveSelectionHighlight();

	Vector<TextSegment> segments;
	Vector<LineLayout> lines;
	String flat_text;
	ElementText* reference_text = nullptr;
	ElementTextSelection* selection_style_element = nullptr;
	int active_selection_start = 0;
	int active_selection_end = 0;
	bool suppress_click = false;
	Geometry handle_start_geometry;
	Geometry handle_end_geometry;
};

} // namespace Rml
