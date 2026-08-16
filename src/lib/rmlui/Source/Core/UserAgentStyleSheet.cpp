#include "UserAgentStyleSheet.h"

#include "../../Include/RmlUi/Core/Log.h"
#include "../../Include/RmlUi/Core/StreamMemory.h"
#include "../../Include/RmlUi/Core/StyleSheetContainer.h"

#include <cstring>

namespace Rml {

namespace {

// Based on the RmlUi-recommended HTML4 style sheet, extended with list elements.
// Intentionally minimal — not a full browser UA sheet. List bullets are NOT handled here;
// see ListMarker.cpp (FORK_WORKAROUND: layout-time marker injection).
constexpr const char* user_agent_rcss = R"rcss(
body, div,
h1, h2, h3, h4, h5, h6,
p, hr, pre,
ul, ol, li,
blockquote,
tabset tabs
{
	display: block;
}

h1
{
	font-size: 2em;
	margin-top: 0.67em;
	margin-bottom: 0.67em;
	font-weight: bold;
}

h2
{
	font-size: 1.5em;
	margin-top: 0.75em;
	margin-bottom: 0.75em;
	font-weight: bold;
}

h3
{
	font-size: 1.17em;
	margin-top: 0.83em;
	margin-bottom: 0.83em;
	font-weight: bold;
}

h4, p
{
	margin-top: 1.12em;
	margin-bottom: 1.12em;
}

h5
{
	font-size: 0.83em;
	margin-top: 1.5em;
	margin-bottom: 1.5em;
	font-weight: bold;
}

h6
{
	font-size: 0.75em;
	margin-top: 1.67em;
	margin-bottom: 1.67em;
	font-weight: bold;
}

em
{
	font-style: italic;
}

strong
{
	font-weight: bold;
}

pre
{
	white-space: pre;
}

ul, ol
{
	box-sizing: border-box;
	margin-top: 1em;
	margin-bottom: 1em;
	padding-left: 40dp;
}

li
{
	margin-bottom: 0.25em;
}

ul ul, ul ol, ol ul, ol ol
{
	margin-top: 0;
	margin-bottom: 0;
}

table
{
	box-sizing: border-box;
	display: table;
}

tr
{
	box-sizing: border-box;
	display: table-row;
}

td
{
	box-sizing: border-box;
	display: table-cell;
}

col
{
	box-sizing: border-box;
	display: table-column;
}

colgroup
{
	display: table-column-group;
}

thead, tbody, tfoot
{
	display: table-row-group;
}

/* Scrollbar widgets (ElementScroll). Without explicit sizes the layout box can span the full
   scroll container while only the thumb is drawn, stealing pointer hits from content. */
scrollbarvertical
{
	width: 16dp;
}
scrollbarhorizontal
{
	height: 16dp;
}

textarea,
input
{
	cursor: text;
}
)rcss";

SharedPtr<StyleSheetContainer> user_agent_style_sheet;

} // namespace

bool UserAgentStyleSheet::Initialise()
{
	RMLUI_ASSERT(!user_agent_style_sheet);

	auto sheet = MakeShared<StyleSheetContainer>();
	auto stream = MakeUnique<StreamMemory>((const byte*)user_agent_rcss, strlen(user_agent_rcss));
	stream->SetSourceURL("user-agent.rcss");

	if (!sheet->LoadStyleSheetContainer(stream.get()))
	{
		Log::Message(Log::LT_ERROR, "Failed to load built-in user-agent stylesheet.");
		return false;
	}

	user_agent_style_sheet = std::move(sheet);
	return true;
}

void UserAgentStyleSheet::Shutdown()
{
	user_agent_style_sheet.reset();
}

const StyleSheetContainer* UserAgentStyleSheet::GetStyleSheetContainer()
{
	return user_agent_style_sheet.get();
}

} // namespace Rml
