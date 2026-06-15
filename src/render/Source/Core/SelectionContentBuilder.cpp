#include "SelectionContentBuilder.h"

#include "../../Include/RmlUi/Core/ElementText.h"

namespace Rml {

void SelectionContentBuilder::BeginContainer(Element* new_container)
{
	container = new_container;
	flat_text.clear();
	segments.clear();
}

void SelectionContentBuilder::EndContainer()
{
	container = nullptr;
}

void SelectionContentBuilder::AppendText(ElementText* text, const String& content)
{
	if (!text || content.empty())
		return;

	const int begin = (int)flat_text.size();
	flat_text += content;
	if (begin < (int)flat_text.size())
		segments.push_back({text, begin, (int)flat_text.size()});
}

void SelectionContentBuilder::AppendGap()
{
	// Gaps occupy no flat-text space; hit-testing snaps across them.
}

void SelectionContentBuilder::AppendBlockSeparator()
{
	if (!flat_text.empty() && flat_text.back() != '\n')
		flat_text += '\n';
}

} // namespace Rml
