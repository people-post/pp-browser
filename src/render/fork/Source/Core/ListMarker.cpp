#include "ListMarker.h"

#include "../../Include/RmlUi/Core/Element.h"

// FORK_WORKAROUND — see ListMarker.h and docs/RMLUI_UPSTREAM.md.

namespace Rml {

String GetListItemMarker(Element* list_item_element)
{
	if (!list_item_element || list_item_element->GetTagName() != "li")
		return {};

	Element* list = list_item_element->GetParentNode();
	if (!list)
		return {};

	const String& list_tag = list->GetTagName();
	if (list_tag != "ul" && list_tag != "ol")
		return {};

	int index = 0;
	for (int i = 0; i < list->GetNumChildren(); i++)
	{
		Element* child = list->GetChild(i);
		if (child->GetTagName() != "li")
			continue;

		index++;
		if (child == list_item_element)
		{
			if (list_tag == "ol")
				return CreateString("%d. ", index);
			return String("\xE2\x80\xA2 ");
		}
	}

	return {};
}

} // namespace Rml
