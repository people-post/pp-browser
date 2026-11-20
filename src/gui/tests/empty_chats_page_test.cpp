// The empty Chats page says what to do and offers the Add contact button (a 44dp primary button), only when the list is
// empty because there are no chats (not while searching or looking at the archive).
#include "foundation/i18n/LocalizationService.h"

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

TEST(EmptyChatsPageTest, ThePrimaryButtonOpensAddContactAndShowsOnlyForAnEmptyUnfilteredList) {
  const std::string rml = ReadAsset("views/sidebar.rml");
  const size_t block = rml.find("class=\"sidebar-empty\"");
  ASSERT_NE(block, std::string::npos);
  const std::string tag = rml.substr(rml.rfind('<', block), rml.find('>', block) - rml.rfind('<', block));
  EXPECT_NE(tag.find("sessions.size == 0 && sessions_query == '' && !showing_archived"), std::string::npos);
  const std::string inner = rml.substr(block, rml.find("</div>", block) - block);
  EXPECT_NE(inner.find("class=\"btn btn-primary\""), std::string::npos);
  EXPECT_NE(inner.find("data-event-click=\"empty_add_contact()\""), std::string::npos);
  EXPECT_NE(inner.find("{{i18n:contacts.add}}"), std::string::npos);
  EXPECT_NE(inner.find("{{i18n:sidebar.empty_start}}"), std::string::npos);
  // The other empty states (a search that matches nothing, the archive) keep the plain line.
  EXPECT_NE(rml.find("sessions_query != '' || showing_archived)\">{{i18n:sidebar.empty}}"), std::string::npos);
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(css.find("min-height: 44dp;"), std::string::npos) << ".btn is at least 44dp tall";
}

TEST(EmptyChatsPageTest, TheLinesAreTranslated) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("sidebar.empty_start"), "还没有聊天。添加一个联系人就能开始。");
  EXPECT_EQ(Tr("contacts.add"), "添加联系人");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
