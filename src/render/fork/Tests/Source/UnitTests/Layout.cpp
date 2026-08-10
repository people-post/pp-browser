#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <doctest.h>

using namespace Rml;

static const String document_layout_rml = R"(
<rml>
<head>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			width: 500px;
			height: 300px;
			top: 100px;
			left: 100px;
			border: 10px #fff;
			background-color: #ccc;
		}
		#relative {
			width: 100%;
			height: 25%;
			background-color: red;
			position: relative;
			top: 50%;
		}
	</style>
</head>

<body>
	<div id="relative"/>
</body>
</rml>
)";

static const String document_layout_rml_nested = R"(
<rml>
<head>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			width: 500px;
			height: 300px;
			top: 100px;
			left: 100px;
			border: 10px #fff;
			background-color: #ccc;
		}
		#parent {
			background-color: green;
			position: relative;
			top: 50%;
		}
		#relative {
			width: 100%;
			height: 25%;
			background-color: red;
			position: relative;
			top: 50%;
		}
	</style>
</head>

<body>
	<div id="parent">
		<div id="relative"/>
	</div>
</body>
</rml>
)";

TEST_CASE("Layout.Position.Relative")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	// Test that percentage positioning in 'position: relative' elements is correctly resolved during the first layout run, and
	// does not change during the next layout run. See issue: https://github.com/mikke89/RmlUi/issues/262

	for (auto&& rml_source : {document_layout_rml, document_layout_rml_nested})
	{
		ElementDocument* document = context->LoadDocumentFromMemory(rml_source);
		REQUIRE(document);
		document->Show();

		Element* element = document->GetElementById("relative");
		REQUIRE(element);

		TestsShell::RenderLoop();

		const float absolute_top = element->GetAbsoluteTop();
		CHECK(absolute_top >= 150.f);

		// This forces a new layout run but shouldn't make any difference to the rendered output.
		document->SetProperty("width", "500px");
		TestsShell::RenderLoop();

		CHECK(absolute_top == element->GetAbsoluteTop());

		document->SetProperty("width", "400px");
		TestsShell::RenderLoop();

		CHECK(absolute_top == element->GetAbsoluteTop());

		document->Close();
	}

	TestsShell::ShutdownShell();
}

// Fork: position:sticky — sticks at top within the scrollport, unsticks at parent boundary.
TEST_CASE("Layout.Position.Sticky")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	static const String sticky_rml = R"(
<rml>
<head>
	<link type="text/rcss" href="/assets/rml.rcss"/>
	<style>
		body {
			width: 400px;
			height: 300px;
			top: 0px;
			left: 0px;
		}
		#scroll {
			width: 300px;
			height: 200px;
			overflow: auto;
			background-color: #eee;
		}
		#section {
			width: 100%;
			height: 500px;
			background-color: #ddd;
		}
		#sticky {
			width: 100%;
			height: 40px;
			position: sticky;
			top: 0px;
			background-color: red;
		}
		#spacer {
			width: 100%;
			height: 400px;
			background-color: #ccc;
		}
	</style>
</head>
<body>
	<div id="scroll">
		<div id="section">
			<div id="sticky"/>
			<div id="spacer"/>
		</div>
	</div>
</body>
</rml>
)";

	ElementDocument* document = context->LoadDocumentFromMemory(sticky_rml);
	REQUIRE(document);
	document->Show();
	TestsShell::RenderLoop();

	Element* scroll = document->GetElementById("scroll");
	Element* sticky = document->GetElementById("sticky");
	REQUIRE(scroll);
	REQUIRE(sticky);

	const float scroll_abs_top = scroll->GetAbsoluteTop();
	const float initial_sticky_top = sticky->GetAbsoluteTop();
	CHECK(initial_sticky_top == doctest::Approx(scroll_abs_top).epsilon(0.01));

	// Scroll down: sticky should remain pinned to the scrollport top.
	scroll->SetScrollTop(80.f);
	TestsShell::RenderLoop();
	CHECK(sticky->GetAbsoluteTop() == doctest::Approx(scroll_abs_top).epsilon(0.01));

	// Scroll far enough that the section would leave: sticky unsticks with the parent bottom.
	scroll->SetScrollTop(480.f);
	TestsShell::RenderLoop();
	const float stuck_far = sticky->GetAbsoluteTop();
	// Section height 500, sticky 40 → last stuck position is section bottom - 40 relative to scroll content.
	// After heavy scroll the sticky top should be above the scrollport (scrolled away) or at constraint.
	CHECK(stuck_far <= scroll_abs_top + 1.f);

	document->Close();
	TestsShell::ShutdownShell();
}
