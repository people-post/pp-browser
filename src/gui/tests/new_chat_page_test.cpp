// Phone "New chat" page: the markup binds what ContactsController provides, and its rows have the agreed sizes.
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

bool Before(const std::string& text, const std::string& a, const std::string& b) {
  const size_t pa = text.find(a);
  const size_t pb = text.find(b);
  return pa != std::string::npos && pb != std::string::npos && pa < pb;
}

TEST(NewChatPageTest, ViewIsRegisteredUnderItsKey) {
  EXPECT_EQ(ViewCatalog::ResolvePath("new_chat"), "views/new_chat.rml");
  EXPECT_FALSE(ReadAsset("views/new_chat.rml").empty());
}

TEST(NewChatPageTest, SearchActionsFrequentAndLettersComeInThatOrder) {
  const std::string rml = ReadAsset("views/new_chat.rml");
  EXPECT_NE(rml.find("data-value=\"nc_query\""), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:sidebar.search_placeholder}}"), std::string::npos);
  EXPECT_TRUE(Before(rml, "chat-search nc-search", "new_chat_action('ai')"));
  EXPECT_TRUE(Before(rml, "new_chat_action('ai')", "new_chat_action('group')"));
  EXPECT_TRUE(Before(rml, "new_chat_action('group')", "new_chat_action('add')"));
  EXPECT_TRUE(Before(rml, "new_chat_action('add')", "data-for=\"contact : nc_frequent\""));
  EXPECT_TRUE(Before(rml, "data-for=\"contact : nc_frequent\"", "data-for=\"section : nc_sections\""));
  EXPECT_NE(rml.find("data-for=\"contact : section.rows\""), std::string::npos);
  EXPECT_NE(rml.find("new_chat_pick(contact.id)"), std::string::npos);
}

TEST(NewChatPageTest, ActionRowsAreBigAndTheRowsAreTheContactsTabRows) {
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t action = css.find(".nc-action {");
  ASSERT_NE(action, std::string::npos);
  EXPECT_NE(css.find("height: 56dp;", action), std::string::npos);
  const size_t icon = css.find(".nc-action-icon {");
  ASSERT_NE(icon, std::string::npos);
  EXPECT_NE(css.find("width: 36dp;", icon), std::string::npos);
  // The contact rows reuse the Contacts tab's 60dp phone row.
  EXPECT_NE(ReadAsset("views/new_chat.rml").find("class=\"contacts-row\""), std::string::npos);
  EXPECT_NE(css.find("height: 60dp;", css.find(".shell-layer-compact .contacts-row {")), std::string::npos);
}

TEST(NewChatPageTest, ColoursComeFromTokensOnly) {
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t begin = css.find(".nc-page {");
  const size_t end = css.find("/* Contact detail, phone. */");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  const std::string block = css.substr(begin, end - begin);
  EXPECT_EQ(block.find("color:"), std::string::npos);
  EXPECT_EQ(block.find('#'), std::string::npos);
}

} // namespace
} // namespace pbr
