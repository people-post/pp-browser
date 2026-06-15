#pragma once

#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/EventListener.h"
#include "../../../Include/RmlUi/Core/Geometry.h"
#include "../../../Include/RmlUi/Core/Input.h"
#include "../../../Include/RmlUi/Core/SelectionTypes.h"

namespace Rml {

class ElementText;
class SelectionContentBuilder;

/// Static text container with drag-selection and Ctrl+C copy. Created for elements with selectable="text".
class ElementSelectableText : public Element, public EventListener {
public:
	RMLUI_RTTI_DefineWithParent(ElementSelectableText, Element)

	explicit ElementSelectableText(const String& tag);
	~ElementSelectableText() override;

	void OnRender() override;

	SelectionDisposition QuerySelection(const SelectionQuery& query) override;
	void BuildSelectionContent(SelectionContentBuilder& builder) override;
	SelectionEndpoint HitTestSelection(Vector2f absolute_position) const override;
	void RenderSelectionSlice(int local_start, int local_end) override;
	String GetSelectionSlice(int local_start, int local_end) const override;

	bool IsSelectionRoot() const;
	void RefreshSelectionContent();
	const String& GetFlatText() const { return flat_text; }
	int HitTestLocal(Vector2f absolute_mouse);
	void UpdateSelectionHighlight(int local_start, int local_end);
	void ClearSelectionHighlight();

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
	void BuildSelectionGeometry(int local_start, int local_end);

	Vector<TextSegment> segments;
	Vector<LineLayout> lines;
	String flat_text;
	ElementText* reference_text = nullptr;
	Geometry selection_geometry;
	bool suppress_click = false;
};

} // namespace Rml
