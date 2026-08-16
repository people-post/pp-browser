#pragma once

#include "../../Include/RmlUi/Core/SelectionTypes.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
class ElementText;

class SelectionContentBuilder {
public:
	struct TextSegment {
		ElementText* element = nullptr;
		int flat_begin = 0;
		int flat_end = 0;
	};

	void BeginContainer(Element* container);
	void EndContainer();

	void AppendText(ElementText* text, const String& content);
	void AppendGap();
	void AppendBlockSeparator();

	Element* GetContainer() const { return container; }
	const String& GetFlatText() const { return flat_text; }
	const Vector<TextSegment>& GetSegments() const { return segments; }

private:
	Element* container = nullptr;
	String flat_text;
	Vector<TextSegment> segments;
};

} // namespace Rml
