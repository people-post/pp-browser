#include "ClickRouting.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>

#include <doctest.h>

#include <unordered_map>

namespace {

Rml::SystemInterface g_system_interface;
std::unordered_map<Rml::Element*, bool> g_contains_point;
bool g_rmlui_initialized = false;

void EnsureRmlUiInitialized()
{
	if (!g_rmlui_initialized)
	{
		Rml::SetSystemInterface(&g_system_interface);
		REQUIRE(Rml::Initialise());
		g_rmlui_initialized = true;
	}
}

Rml::ElementPtr MakeElement(const char* tag)
{
	Rml::ElementPtr element = Rml::Factory::InstanceElement(nullptr, "*", tag, Rml::XMLAttributes());
	REQUIRE(static_cast<bool>(element));
	return element;
}

Rml::Element* AppendChild(Rml::Element& parent, const char* tag)
{
	Rml::ElementPtr child = Rml::Factory::InstanceElement(&parent, "*", tag, Rml::XMLAttributes());
	REQUIRE(static_cast<bool>(child));
	Rml::Element* inserted = parent.AppendChild(std::move(child), false);
	REQUIRE(inserted != nullptr);
	return inserted;
}

bool MockPointWithin(Rml::Element* element, Rml::Vector2f /*point*/, void* /*context*/)
{
	const auto it = g_contains_point.find(element);
	if (it == g_contains_point.end())
		return false;
	return it->second;
}

void SetContains(Rml::Element* element, bool contains) { g_contains_point[element] = contains; }

Rml::Element* NullFocus(Rml::Element* /*element*/) { return nullptr; }

Rml::Element* g_focus_result = nullptr;
Rml::Element* ReturnFocusResult(Rml::Element* /*element*/) { return g_focus_result; }

Rml::Element* Resolve(Rml::Element* press_hover, Rml::Element* release_hover, Rml::Vector2f point,
	Rml::ClickRouting::FindFocusElementFn find_focus)
{
	return Rml::ClickRouting::ResolveClickTargetWithPredicate(press_hover, release_hover, point, find_focus, MockPointWithin, nullptr);
}

} // namespace

TEST_CASE("ClickRouting.TreeHelpers")
{
	EnsureRmlUiInitialized();
	g_contains_point.clear();

	Rml::ElementPtr root_ptr = MakeElement("div");
	REQUIRE(static_cast<bool>(root_ptr));
	Rml::Element& root = *root_ptr;
	Rml::Element* child = AppendChild(root, "span");
	REQUIRE(child != nullptr);
	Rml::Element* grandchild = AppendChild(*child, "option");
	REQUIRE(grandchild != nullptr);

	CHECK(Rml::ClickRouting::IsAncestorOf(&root, grandchild));
	CHECK(Rml::ClickRouting::IsAncestorOf(child, grandchild));
	CHECK_FALSE(Rml::ClickRouting::IsAncestorOf(grandchild, &root));

	CHECK(Rml::ClickRouting::InSameClickTree(grandchild, grandchild));
	CHECK(Rml::ClickRouting::InSameClickTree(child, grandchild));
	CHECK(Rml::ClickRouting::InSameClickTree(grandchild, child));

	Rml::Element* sibling_b = AppendChild(root, "span");
	REQUIRE(sibling_b != nullptr);
	CHECK_FALSE(Rml::ClickRouting::InSameClickTree(child, sibling_b));
}

TEST_CASE("ClickRouting.FindInteractiveElement")
{
	EnsureRmlUiInitialized();
	g_contains_point.clear();

	Rml::ElementPtr div_ptr = MakeElement("div");
	REQUIRE(static_cast<bool>(div_ptr));
	Rml::Element& div = *div_ptr;
	Rml::Element* button = AppendChild(div, "button");
	REQUIRE(button != nullptr);
	Rml::Element* text = AppendChild(*button, "span");
	REQUIRE(text != nullptr);

	CHECK(Rml::ClickRouting::FindInteractiveElement(text) == button);

	Rml::ElementPtr scrim_ptr = MakeElement("div");
	REQUIRE(static_cast<bool>(scrim_ptr));
	Rml::Element& scrim = *scrim_ptr;
	scrim.SetAttribute("data-event-click", "close()");
	CHECK(Rml::ClickRouting::FindInteractiveElement(&scrim) == &scrim);
}

TEST_CASE("ClickRouting.ResolveClickTargetTier1Option")
{
	EnsureRmlUiInitialized();
	g_contains_point.clear();

	Rml::ElementPtr select_ptr = MakeElement("select");
	REQUIRE(static_cast<bool>(select_ptr));
	Rml::Element& select = *select_ptr;
	Rml::Element* selectbox = AppendChild(select, "selectbox");
	REQUIRE(selectbox != nullptr);
	Rml::Element* option = AppendChild(*selectbox, "option");
	REQUIRE(option != nullptr);

	const Rml::Vector2f point{10.f, 10.f};
	CHECK(Resolve(option, option, point, NullFocus) == option);
}

TEST_CASE("ClickRouting.ResolveClickTargetTier1ButtonChild")
{
	EnsureRmlUiInitialized();
	g_contains_point.clear();

	Rml::ElementPtr button_ptr = MakeElement("button");
	REQUIRE(static_cast<bool>(button_ptr));
	Rml::Element& button = *button_ptr;
	Rml::Element* label = AppendChild(button, "span");
	REQUIRE(label != nullptr);

	const Rml::Vector2f point{4.f, 4.f};
	CHECK(Resolve(label, label, point, NullFocus) == label);
}

TEST_CASE("ClickRouting.ResolveClickTargetTier2SiblingChildren")
{
	EnsureRmlUiInitialized();
	g_contains_point.clear();

	Rml::ElementPtr button_ptr = MakeElement("button");
	REQUIRE(static_cast<bool>(button_ptr));
	Rml::Element& button = *button_ptr;
	Rml::Element* press = AppendChild(button, "span");
	Rml::Element* release = AppendChild(button, "span");
	REQUIRE(press != nullptr);
	REQUIRE(release != nullptr);

	SetContains(&button, true);
	SetContains(press, false);
	SetContains(release, false);

	const Rml::Vector2f point{8.f, 8.f};
	CHECK(Resolve(press, release, point, NullFocus) == &button);
}

TEST_CASE("ClickRouting.ResolveClickTargetUnrelated")
{
	EnsureRmlUiInitialized();
	g_contains_point.clear();

	Rml::ElementPtr press_ptr = MakeElement("div");
	Rml::ElementPtr release_ptr = MakeElement("div");
	REQUIRE(static_cast<bool>(press_ptr));
	REQUIRE(static_cast<bool>(release_ptr));
	Rml::Element& press = *press_ptr;
	Rml::Element& release = *release_ptr;
	SetContains(&press, true);
	SetContains(&release, true);

	const Rml::Vector2f point{1.f, 1.f};
	CHECK(Resolve(&press, &release, point, NullFocus) == nullptr);
}

TEST_CASE("ClickRouting.ResolveClickTargetTier3Geometry")
{
	EnsureRmlUiInitialized();
	g_contains_point.clear();

	Rml::ElementPtr press_ptr = MakeElement("div");
	REQUIRE(static_cast<bool>(press_ptr));
	Rml::Element& press = *press_ptr;
	SetContains(&press, true);

	const Rml::Vector2f point{2.f, 2.f};
	CHECK(Resolve(&press, nullptr, point, NullFocus) == &press);
}

TEST_CASE("ClickRouting.ResolveClickTargetTier3Focus")
{
	EnsureRmlUiInitialized();
	g_contains_point.clear();

	Rml::ElementPtr press_ptr = MakeElement("div");
	Rml::ElementPtr release_ptr = MakeElement("span");
	REQUIRE(static_cast<bool>(press_ptr));
	REQUIRE(static_cast<bool>(release_ptr));
	Rml::Element& press = *press_ptr;
	Rml::Element& release = *release_ptr;
	g_focus_result = &press;
	SetContains(&press, false);

	const Rml::Vector2f point{0.f, 0.f};
	CHECK(Resolve(&press, &release, point, ReturnFocusResult) == &press);
	g_focus_result = nullptr;
}
