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

TEST(HomeEditMarkupTest, EditHomeIsAPageSheetWithRemoveAndAddRows) {
  EXPECT_EQ(ViewCatalog::ResolvePath("home_edit"), "views/home_edit.rml");
  const std::string rml = ReadAsset("views/home_edit.rml");
  ASSERT_FALSE(rml.empty());
  EXPECT_TRUE(Has(rml, "class=\"sheet-header\"")) << "the shared sheet title row";
  EXPECT_TRUE(Has(rml, "data-for=\"row : home_edit_on\""));
  EXPECT_TRUE(Has(rml, "data-for=\"row : home_edit_off\""));
  EXPECT_TRUE(Has(rml, "home_widget_remove(row.id)"));
  EXPECT_TRUE(Has(rml, "home_widget_add(row.id)"));
  EXPECT_TRUE(Has(rml, "{{row.size}}")) << "wide or small";
  // A drag handle on each row on Home reorders them; the dragged row is lifted.
  EXPECT_TRUE(Has(rml, "data-event-dragstart=\"home_edit_drag_start(row.id)\""));
  EXPECT_TRUE(Has(rml, "data-event-drag=\"home_edit_drag()\""));
  EXPECT_TRUE(Has(rml, "data-event-dragend=\"home_edit_drag_end()\""));
  EXPECT_TRUE(Has(rml, "data-class-home-edit-row--lifted=\"row.id == home_edit_dragging\""));
  const std::string rcss = ReadAsset("themes/components.rcss");
  const size_t handle = rcss.find(".home-edit-handle {");
  ASSERT_NE(handle, std::string::npos);
  EXPECT_NE(rcss.find("drag: drag;", handle), std::string::npos) << "the engine sends it drag events";
}

TEST(HomeEditMarkupTest, HomeDrawsTheWidgetsInTheUsersOrderAndHasAnEditButton) {
  const std::string rml = ReadAsset("views/home.rml");
  EXPECT_TRUE(Has(rml, "data-for=\"w : home_widgets\""));
  EXPECT_TRUE(Has(rml, "data-class-home-slot--wide=\"w.wide\""));
  for (const std::string& id : HomeWidgetIds()) {
    EXPECT_TRUE(Has(rml, "data-if=\"w.id == '" + id + "'\"")) << id;
  }
  EXPECT_TRUE(Has(rml, "home_edit_open()"));
  EXPECT_TRUE(Has(rml, "home_open_favorite(f_index)"));
  EXPECT_TRUE(Has(rml, "home_open_calls()"));
  EXPECT_TRUE(Has(rml, "home_open_mentions()"));
  EXPECT_TRUE(Has(rml, "home_open_new_people()"));
  EXPECT_TRUE(Has(rml, "home_open_brief(item_index)"));
  EXPECT_TRUE(Has(rml, "home_open_chats()"));
  // Me -> Appearance opens the same sheet.
  EXPECT_TRUE(Has(ReadAsset("views/settings_section_appearance.rml"), "open_home_edit()"));
}

} // namespace
} // namespace pbr
