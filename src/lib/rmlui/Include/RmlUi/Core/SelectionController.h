#pragma once

#include "Header.h"
#include "Input.h"
#include "Types.h"
#include "Vector2.h"

namespace Rml {

class Context;
class Element;

class RMLUICORE_API SelectionController {
public:
	bool CanSelectStaticText(Element* target) const;
	bool BlocksTarget(Element* target) const;
	void SelectWordAt(Vector2i position);
	void SelectAll();
	void FinalizeSelection();
	void OnPointerUp();
	String GetSelectedText();
	bool HasSelection() const;
};

} // namespace Rml
