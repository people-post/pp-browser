#include "ClickRouting.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>

#include <gtest/gtest.h>

#include <unordered_map>

namespace {

Rml::SystemInterface g_system_interface;
std::unordered_map<Rml::Element*, bool> g_contains_point;

Rml::ElementPtr MakeElement(const char* tag)
{
	Rml::ElementPtr element = Rml::Factory::InstanceElement(nullptr, "*", tag, Rml::XMLAttributes());
	EXPECT_TRUE(static_cast<bool>(element));
	return element;
}

Rml::Element* AppendChild(Rml::Element& parent, const char* tag)
{
	Rml::ElementPtr child = Rml::Factory::InstanceElement(&parent, "*", tag, Rml::XMLAttributes());
	EXPECT_TRUE(static_cast<bool>(child));
	Rml::Element* inserted = parent.AppendChild(std::move(child), false);
	EXPECT_NE(inserted, nullptr);
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
	return Rml::ClickRouting::ResolveClickTargetWithPredicate(press_hover, release_hover, point, find_focus,
	                                                          MockPointWithin, nullptr);
}

class ClickRoutingTest : public ::testing::Test {
protected:
	static void SetUpTestSuite()
	{
		Rml::SetSystemInterface(&g_system_interface);
		ASSERT_TRUE(Rml::Initialise());
	}

	static void TearDownTestSuite() { Rml::Shutdown(); }

	void SetUp() override { g_contains_point.clear(); }
};

} // namespace

TEST_F(ClickRoutingTest, TreeHelpers)
{
	Rml::ElementPtr root_ptr = MakeElement("div");
	ASSERT_TRUE(static_cast<bool>(root_ptr));
	Rml::Element& root = *root_ptr;
	Rml::Element* child = AppendChild(root, "span");
	ASSERT_NE(child, nullptr);
	Rml::Element* grandchild = AppendChild(*child, "option");
	ASSERT_NE(grandchild, nullptr);

	EXPECT_TRUE(Rml::ClickRouting::IsAncestorOf(&root, grandchild));
	EXPECT_TRUE(Rml::ClickRouting::IsAncestorOf(child, grandchild));
	EXPECT_FALSE(Rml::ClickRouting::IsAncestorOf(grandchild, &root));

	EXPECT_TRUE(Rml::ClickRouting::InSameClickTree(grandchild, grandchild));
	EXPECT_TRUE(Rml::ClickRouting::InSameClickTree(child, grandchild));
	EXPECT_TRUE(Rml::ClickRouting::InSameClickTree(grandchild, child));

	Rml::Element* sibling_b = AppendChild(root, "span");
	ASSERT_NE(sibling_b, nullptr);
	EXPECT_FALSE(Rml::ClickRouting::InSameClickTree(child, sibling_b));
}

TEST_F(ClickRoutingTest, FindInteractiveElement)
{
	Rml::ElementPtr div_ptr = MakeElement("div");
	ASSERT_TRUE(static_cast<bool>(div_ptr));
	Rml::Element& div = *div_ptr;
	Rml::Element* button = AppendChild(div, "button");
	ASSERT_NE(button, nullptr);
	Rml::Element* text = AppendChild(*button, "span");
	ASSERT_NE(text, nullptr);

	EXPECT_EQ(Rml::ClickRouting::FindInteractiveElement(text), button);

	Rml::ElementPtr scrim_ptr = MakeElement("div");
	ASSERT_TRUE(static_cast<bool>(scrim_ptr));
	Rml::Element& scrim = *scrim_ptr;
	scrim.SetAttribute("data-event-click", "close()");
	EXPECT_EQ(Rml::ClickRouting::FindInteractiveElement(&scrim), &scrim);
}

TEST_F(ClickRoutingTest, ResolveClickTargetTier1Option)
{
	Rml::ElementPtr select_ptr = MakeElement("select");
	ASSERT_TRUE(static_cast<bool>(select_ptr));
	Rml::Element& select = *select_ptr;
	Rml::Element* selectbox = AppendChild(select, "selectbox");
	ASSERT_NE(selectbox, nullptr);
	Rml::Element* option = AppendChild(*selectbox, "option");
	ASSERT_NE(option, nullptr);

	const Rml::Vector2f point{10.f, 10.f};
	EXPECT_EQ(Resolve(option, option, point, NullFocus), option);
}

TEST_F(ClickRoutingTest, ResolveClickTargetTier1ButtonChild)
{
	Rml::ElementPtr button_ptr = MakeElement("button");
	ASSERT_TRUE(static_cast<bool>(button_ptr));
	Rml::Element& button = *button_ptr;
	Rml::Element* label = AppendChild(button, "span");
	ASSERT_NE(label, nullptr);

	const Rml::Vector2f point{4.f, 4.f};
	EXPECT_EQ(Resolve(label, label, point, NullFocus), label);
}

TEST_F(ClickRoutingTest, ResolveClickTargetTier2SiblingChildren)
{
	Rml::ElementPtr button_ptr = MakeElement("button");
	ASSERT_TRUE(static_cast<bool>(button_ptr));
	Rml::Element& button = *button_ptr;
	Rml::Element* press = AppendChild(button, "span");
	Rml::Element* release = AppendChild(button, "span");
	ASSERT_NE(press, nullptr);
	ASSERT_NE(release, nullptr);

	SetContains(&button, true);
	SetContains(press, false);
	SetContains(release, false);

	const Rml::Vector2f point{8.f, 8.f};
	EXPECT_EQ(Resolve(press, release, point, NullFocus), &button);
}

TEST_F(ClickRoutingTest, ResolveClickTargetUnrelated)
{
	Rml::ElementPtr press_ptr = MakeElement("div");
	Rml::ElementPtr release_ptr = MakeElement("div");
	ASSERT_TRUE(static_cast<bool>(press_ptr));
	ASSERT_TRUE(static_cast<bool>(release_ptr));
	Rml::Element& press = *press_ptr;
	Rml::Element& release = *release_ptr;
	SetContains(&press, true);
	SetContains(&release, true);

	const Rml::Vector2f point{1.f, 1.f};
	EXPECT_EQ(Resolve(&press, &release, point, NullFocus), nullptr);
}

TEST_F(ClickRoutingTest, ResolveClickTargetTier3Geometry)
{
	Rml::ElementPtr press_ptr = MakeElement("div");
	ASSERT_TRUE(static_cast<bool>(press_ptr));
	Rml::Element& press = *press_ptr;
	SetContains(&press, true);

	const Rml::Vector2f point{2.f, 2.f};
	EXPECT_EQ(Resolve(&press, nullptr, point, NullFocus), &press);
}

TEST_F(ClickRoutingTest, ResolveClickTargetTier3Focus)
{
	Rml::ElementPtr press_ptr = MakeElement("div");
	Rml::ElementPtr release_ptr = MakeElement("span");
	ASSERT_TRUE(static_cast<bool>(press_ptr));
	ASSERT_TRUE(static_cast<bool>(release_ptr));
	Rml::Element& press = *press_ptr;
	Rml::Element& release = *release_ptr;
	g_focus_result = &press;
	SetContains(&press, false);

	const Rml::Vector2f point{0.f, 0.f};
	EXPECT_EQ(Resolve(&press, &release, point, ReturnFocusResult), &press);
	g_focus_result = nullptr;
}
