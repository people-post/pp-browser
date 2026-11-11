// The reactions sheet (a tap on a message's emoji lists who reacted, like WhatsApp): its view, the markup and the
// words it needs in both languages.
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

TEST(ReactorsSheetTest, TheViewIsRegisteredAndListsPeopleWithOnlyTheOwnRowTappable) {
  EXPECT_EQ(ViewCatalog::ResolvePath("reactors_sheet"), "views/reactors_sheet.rml");
  const std::string rml = ReadAsset("views/reactors_sheet.rml");
  EXPECT_NE(rml.find("data-for=\"row : reactor_rows\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"reactor_remove(row.emoji)\""), std::string::npos);
  // That one click sits on the own-reaction row, never on the others'.
  const size_t remove = rml.find("reactor_remove(row.emoji)");
  const size_t others = rml.find("data-if=\"!row.is_self\"");
  ASSERT_NE(others, std::string::npos);
  EXPECT_LT(remove, others);
  EXPECT_EQ(rml.find("reactor_remove", others), std::string::npos);
}

// The sheet skin (grouped background, edge-to-edge padding, raised cards in dark) is keyed on the pane's key; without
// it the sheet showed white margins and a grey inset panel unlike the other page sheets.
TEST(ReactorsSheetTest, TheSheetIsSkinnedLikeTheOtherPageSheets) {
  const std::string rml = ReadAsset("views/reactors_sheet.rml");
  EXPECT_NE(rml.find("class=\"ac-page\""), std::string::npos);
  EXPECT_NE(rml.find("<p class=\"sheet-title\">{{i18n:chat.reactions.title}}</p>"), std::string::npos);
  for (const char* file : {"themes/colors.rcss.tmpl", "themes/components.rcss"}) {
    const std::string css = ReadAsset(file);
    EXPECT_NE(css.find("--reactors_sheet"), std::string::npos) << file;
  }
  for (const char* file : {"themes/colors-light.rcss", "themes/colors-dark.rcss"}) {
    EXPECT_NE(ReadAsset(file).find(".shell-page-sheet.shell-page-sheet--reactors_sheet,"), std::string::npos) << file;
  }
}

TEST(ReactorsSheetTest, TheWordsExistInBothLanguages) {
  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    const std::string json = ReadAsset(locale);
    for (const char* key : {"chat.reactions.title", "chat.reactions.tap_to_remove", "chat.reactions.unknown_member"}) {
      EXPECT_NE(json.find(std::string("\"") + key + "\""), std::string::npos) << locale << " lacks " << key;
    }
  }
}

} // namespace
} // namespace pbr
