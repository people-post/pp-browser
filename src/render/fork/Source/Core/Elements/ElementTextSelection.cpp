#include "ElementTextSelection.h"
#include "../../../Include/RmlUi/Core/PropertyIdSet.h"

namespace Rml {

ElementTextSelection::ElementTextSelection(const String& tag) : Element(tag)
{
	client = nullptr;
}

ElementTextSelection::~ElementTextSelection() {}

void ElementTextSelection::SetClient(SelectionStyleClient* _client)
{
	client = _client;
}

void ElementTextSelection::OnPropertyChange(const PropertyIdSet& changed_properties)
{
	Element::OnPropertyChange(changed_properties);

	if (client == nullptr)
		return;

	if (changed_properties.Contains(PropertyId::Color) || changed_properties.Contains(PropertyId::BackgroundColor))
		client->OnSelectionStyleChanged();
}

} // namespace Rml
