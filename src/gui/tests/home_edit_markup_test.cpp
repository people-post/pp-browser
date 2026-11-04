// Phone Home's widgets and Edit Home (DESIGN-pp-ui 8.16, PLAN-ui-refresh-impl batch 4): the markup wires the
// events ChatController binds, with the widget ids the stored choices use.
#include "feature/presenters/HomeToday.h"
#include "lib/ui/ViewCatalog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

TEST(HomeEditMarkupTest, EditHomeIsAPageSheetWithASwitchPerWidget) {
  EXPECT_EQ(ViewCatalog::ResolvePath("home_edit"), "views/home_edit.rml");
  const std::string rml = ReadAsset("views/home_edit.rml");
  ASSERT_FALSE(rml.empty());
  EXPECT_TRUE(Has(rml, "class=\"sheet-header\"")) << "the shared sheet title row";
  for (const char* id : {kHomeWidgetMessages, kHomeWidgetNewPeople, kHomeWidgetBrief}) {
    EXPECT_TRUE(Has(rml, std::string("home_widget_toggle('") + id + "')")) << id;
  }
  EXPECT_TRUE(Has(rml, "settings-switch--on=\"home_keep_messages\""));
  EXPECT_TRUE(Has(rml, "settings-switch--on=\"home_keep_new_contacts\""));
  EXPECT_TRUE(Has(rml, "settings-switch--on=\"home_keep_brief\""));
}

TEST(HomeEditMarkupTest, HomeShowsEachWidgetOnlyWithSomethingAndHasAnEditButton) {
  const std::string rml = ReadAsset("views/home.rml");
  EXPECT_TRUE(Has(rml, "data-if=\"home_show_messages\""));
  EXPECT_TRUE(Has(rml, "data-if=\"home_show_new_contacts\""));
  EXPECT_TRUE(Has(rml, "data-if=\"home_show_brief\""));
  EXPECT_TRUE(Has(rml, "home_edit_open()"));
  EXPECT_TRUE(Has(rml, "home_open_new_people()"));
  EXPECT_TRUE(Has(rml, "home_open_brief(item_index)"));
  // Me -> Appearance opens the same sheet.
  EXPECT_TRUE(Has(ReadAsset("views/settings_section_appearance.rml"), "open_home_edit()"));
}

} // namespace
} // namespace pbr
