#include "ClickRouting.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <unordered_map>

namespace {

Rml::SystemInterface g_system_interface;
std::unordered_map<Rml::Element*, bool> g_contains_point;

[[noreturn]] void Fail(const char* message)
{
	std::cerr << "click_routing_test failed: " << message << "\n";
	std::exit(1);
}

void Require(bool condition, const char* message)
{
	if (!condition)
		Fail(message);
}

Rml::ElementPtr MakeElement(const char* tag)
{
	Rml::ElementPtr element = Rml::Factory::InstanceElement(nullptr, "*", tag, Rml::XMLAttributes());
	Require(static_cast<bool>(element), "Factory::InstanceElement returned null");
	return element;
}

Rml::Element* AppendChild(Rml::Element& parent, const char* tag)
{
	Rml::ElementPtr child = Rml::Factory::InstanceElement(&parent, "*", tag, Rml::XMLAttributes());
	Require(static_cast<bool>(child), "Factory::InstanceElement child returned null");
	Rml::Element* inserted = parent.AppendChild(std::move(child), false);
	Require(inserted != nullptr, "AppendChild returned null");
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

Rml::Element* Resolve(Rml::Element* press_hover, Rml::Element* release_hover, Rml::Vector2f point, Rml::ClickRouting::FindFocusElementFn find_focus)
{
	return Rml::ClickRouting::ResolveClickTargetWithPredicate(press_hover, release_hover, point, find_focus, MockPointWithin, nullptr);
}

void TestTreeHelpers()
{
	g_contains_point.clear();
	Rml::ElementPtr root_ptr = MakeElement("div");
	Rml::Element& root = *root_ptr;
	Rml::Element* child = AppendChild(root, "span");
	Rml::Element* grandchild = AppendChild(*child, "option");

	assert(Rml::ClickRouting::IsAncestorOf(&root, grandchild));
	assert(Rml::ClickRouting::IsAncestorOf(child, grandchild));
	assert(!Rml::ClickRouting::IsAncestorOf(grandchild, &root));

	assert(Rml::ClickRouting::InSameClickTree(grandchild, grandchild));
	assert(Rml::ClickRouting::InSameClickTree(child, grandchild));
	assert(Rml::ClickRouting::InSameClickTree(grandchild, child));

	Rml::Element* sibling_b = AppendChild(root, "span");
	assert(!Rml::ClickRouting::InSameClickTree(child, sibling_b));
}

void TestFindInteractiveElement()
{
	g_contains_point.clear();
	Rml::ElementPtr div_ptr = MakeElement("div");
	Rml::Element& div = *div_ptr;
	Rml::Element* button = AppendChild(div, "button");
	Rml::Element* text = AppendChild(*button, "span");

	assert(Rml::ClickRouting::FindInteractiveElement(text) == button);

	Rml::ElementPtr scrim_ptr = MakeElement("div");
	Rml::Element& scrim = *scrim_ptr;
	scrim.SetAttribute("data-event-click", "close()");
	assert(Rml::ClickRouting::FindInteractiveElement(&scrim) == &scrim);
}

void TestResolveClickTargetTier1Option()
{
	g_contains_point.clear();
	Rml::ElementPtr select_ptr = MakeElement("select");
	Rml::Element& select = *select_ptr;
	Rml::Element* selectbox = AppendChild(select, "selectbox");
	Rml::Element* option = AppendChild(*selectbox, "option");

	const Rml::Vector2f point{10.f, 10.f};
	Rml::Element* target = Resolve(option, option, point, NullFocus);
	assert(target == option);
}

void TestResolveClickTargetTier1ButtonChild()
{
	g_contains_point.clear();
	Rml::ElementPtr button_ptr = MakeElement("button");
	Rml::Element& button = *button_ptr;
	Rml::Element* label = AppendChild(button, "span");

	const Rml::Vector2f point{4.f, 4.f};
	Rml::Element* target = Resolve(label, label, point, NullFocus);
	assert(target == label);
}

void TestResolveClickTargetTier2SiblingChildren()
{
	g_contains_point.clear();
	Rml::ElementPtr button_ptr = MakeElement("button");
	Rml::Element& button = *button_ptr;
	Rml::Element* press = AppendChild(button, "span");
	Rml::Element* release = AppendChild(button, "span");

	SetContains(&button, true);
	SetContains(press, false);
	SetContains(release, false);

	const Rml::Vector2f point{8.f, 8.f};
	Rml::Element* target = Resolve(press, release, point, NullFocus);
	assert(target == &button);
}

void TestResolveClickTargetUnrelated()
{
	g_contains_point.clear();
	Rml::ElementPtr press_ptr = MakeElement("div");
	Rml::ElementPtr release_ptr = MakeElement("div");
	Rml::Element& press = *press_ptr;
	Rml::Element& release = *release_ptr;
	SetContains(&press, true);
	SetContains(&release, true);

	const Rml::Vector2f point{1.f, 1.f};
	assert(Resolve(&press, &release, point, NullFocus) == nullptr);
}

void TestResolveClickTargetTier3Geometry()
{
	g_contains_point.clear();
	Rml::ElementPtr press_ptr = MakeElement("div");
	Rml::Element& press = *press_ptr;
	SetContains(&press, true);

	const Rml::Vector2f point{2.f, 2.f};
	Rml::Element* target = Resolve(&press, nullptr, point, NullFocus);
	assert(target == &press);
}

void TestResolveClickTargetTier3Focus()
{
	g_contains_point.clear();
	Rml::ElementPtr press_ptr = MakeElement("div");
	Rml::ElementPtr release_ptr = MakeElement("span");
	Rml::Element& press = *press_ptr;
	Rml::Element& release = *release_ptr;
	g_focus_result = &press;
	SetContains(&press, false);

	const Rml::Vector2f point{0.f, 0.f};
	Rml::Element* target = Resolve(&press, &release, point, ReturnFocusResult);
	assert(target == &press);
	g_focus_result = nullptr;
}

} // namespace

int main()
{
	Rml::SetSystemInterface(&g_system_interface);
	if (!Rml::Initialise())
		Fail("Rml::Initialise returned false");

	TestTreeHelpers();
	TestFindInteractiveElement();
	TestResolveClickTargetTier1Option();
	TestResolveClickTargetTier1ButtonChild();
	TestResolveClickTargetTier2SiblingChildren();
	TestResolveClickTargetUnrelated();
	TestResolveClickTargetTier3Geometry();
	TestResolveClickTargetTier3Focus();

	Rml::Shutdown();

	std::cout << "click_routing_test passed\n";
	return 0;
}
