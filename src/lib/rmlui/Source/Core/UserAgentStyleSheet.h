#pragma once

#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class StyleSheetContainer;

/**
    Built-in user-agent stylesheet providing baseline layout for common HTML elements.
    Merged into every document before author stylesheets.

    Fork note: upstream RmlUi has no UA sheet; see docs/architecture/RMLUI_UPSTREAM.md for gaps
    vs browsers (minimal rule set, no opt-out, form controls still in app theme).
 */
class UserAgentStyleSheet {
public:
	static bool Initialise();
	static void Shutdown();

	static const StyleSheetContainer* GetStyleSheetContainer();
};

} // namespace Rml
