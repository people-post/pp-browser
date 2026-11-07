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

// Full-screen page (DEV/PLAN-minimal-nav.md E): a bar with the centred title, the search field under it, only the
// group and add rows (no AI row), no frequent block, the letter groups, the A-Z strip that names its list.
TEST(NewChatPageTest, BarSearchTwoActionsAndLettersComeInThatOrder) {
  const std::string rml = ReadAsset("views/new_chat.rml");
  EXPECT_NE(rml.find("data-value=\"nc_query\""), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:sidebar.search_placeholder}}"), std::string::npos);
  EXPECT_TRUE(Before(rml, "pane-nav nc-bar", "chat-search nc-search"));
  EXPECT_TRUE(Before(rml, "{{i18n:new_chat.title}}", "chat-search nc-search"));
  EXPECT_TRUE(Before(rml, "chat-search nc-search", "new_chat_action('group')"));
  EXPECT_TRUE(Before(rml, "new_chat_action('group')", "new_chat_action('add')"));
  EXPECT_TRUE(Before(rml, "new_chat_action('add')", "data-for=\"section : nc_sections\""));
  EXPECT_NE(rml.find("data-for=\"contact : section.rows\""), std::string::npos);
  EXPECT_NE(rml.find("new_chat_pick(contact.id)"), std::string::npos);
}

TEST(NewChatPageTest, NoAiRowNoFrequentBlock) {
  const std::string rml = ReadAsset("views/new_chat.rml");
  EXPECT_EQ(rml.find("new_chat_action('ai')"), std::string::npos);
  EXPECT_EQ(rml.find("nc-action--ai"), std::string::npos);
  EXPECT_EQ(rml.find("nc_frequent"), std::string::npos);
  EXPECT_EQ(rml.find("new_chat.frequent"), std::string::npos);
}

TEST(NewChatPageTest, IndexStripNamesItsListAndHidesWithoutContacts) {
  const std::string rml = ReadAsset("views/new_chat.rml");
  for (const char* event : {"data-event-mousedown=\"contacts_index_at('new_chat')\"",
                            "data-event-drag=\"contacts_index_at('new_chat')\"",
                            "data-event-mouseup=\"contacts_index_end('new_chat')\"",
                            "data-event-dragend=\"contacts_index_end('new_chat')\""}) {
    EXPECT_NE(rml.find(event), std::string::npos) << event;
  }
  const size_t strip = rml.find("class=\"contacts-index\"");
  ASSERT_NE(strip, std::string::npos);
  const size_t tag_begin = rml.rfind('<', strip);
  const std::string tag = rml.substr(tag_begin, rml.find('>', strip) - tag_begin);
  EXPECT_NE(tag.find("data-if=\"nc_has_contacts"), std::string::npos) << "no contacts: no strip";
  EXPECT_NE(rml.find("data-for=\"item : nc_index\""), std::string::npos);
  // The empty list says how to start.
  EXPECT_TRUE(Before(rml, "data-if=\"!nc_has_contacts", "{{i18n:new_chat.empty_hint}}"));
  EXPECT_NE(ReadAsset("locales/en.json").find("\"new_chat.empty_hint\": \"Add a contact to start chatting\""),
            std::string::npos);
  EXPECT_NE(ReadAsset("locales/zh-Hans.json").find("\"new_chat.empty_hint\": \"添加联系人，开始聊天\""),
            std::string::npos);
}

// The Contacts tab's strip keeps working with no argument.
TEST(NewChatPageTest, ContactsTabStripIsUnchanged) {
  const std::string rml = ReadAsset("views/contacts.rml");
  EXPECT_NE(rml.find("data-event-mousedown=\"contacts_index_at()\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-mouseup=\"contacts_index_end()\""), std::string::npos);
}

// Kenneth 2026-11-07: the Add contact sheet opens with two big cards, scan and my QR code, above the search field.
TEST(NewChatPageTest, AddSheetHasScanAndMyQrCardsAboveTheSearch) {
  const std::string rml = ReadAsset("views/add_contact.rml");
  EXPECT_TRUE(Before(rml, "add_action('scan')", "add_action('qr')"));
  EXPECT_TRUE(Before(rml, "add_action('qr')", "chat-search ac-search"));
  EXPECT_NE(rml.find("{{i18n:contacts.add_sheet.scan}}"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:contacts.add_sheet.my_qr}}"), std::string::npos);
  EXPECT_NE(ReadAsset("locales/zh-Hans.json").find("\"contacts.add_sheet.scan\": \"扫一扫\""), std::string::npos);
  EXPECT_NE(ReadAsset("locales/zh-Hans.json").find("\"contacts.add_sheet.my_qr\": \"我的二维码\""), std::string::npos);
  EXPECT_NE(ReadAsset("locales/en.json").find("\"contacts.add_sheet.scan\": \"Scan\""), std::string::npos);
  EXPECT_NE(ReadAsset("locales/en.json").find("\"contacts.add_sheet.my_qr\": \"My QR code\""), std::string::npos);
}

// Home root: its own placeholder; the in-conversation one is unchanged.
TEST(NewChatPageTest, HomePlaceholderIsNewAndTheConversationOneIsUnchanged) {
  const std::string en = ReadAsset("locales/en.json");
  const std::string zh = ReadAsset("locales/zh-Hans.json");
  EXPECT_NE(en.find("\"home.composer.placeholder\": \"What can I do for you?\""), std::string::npos);
  EXPECT_NE(zh.find("\"home.composer.placeholder\": \"我能为你做点什么？\""), std::string::npos);
  EXPECT_NE(en.find("\"chat.ai_thread.placeholder\": \"Ask anything…\""), std::string::npos);
  EXPECT_NE(zh.find("\"chat.ai_thread.placeholder\": \"随便问点什么…\""), std::string::npos);
  const std::string composer = ReadAsset("views/composer.rml");
  EXPECT_NE(composer.find("prompt-composer-label--thread\" data-if=\"draft == '' && thread_is_ai\">{{draft_placeholder}}"),
            std::string::npos);
  EXPECT_NE(composer.find("{{i18n:home.composer.placeholder}}"), std::string::npos);
  // The Home label shows only inside the Home mount; the thread one hides there.
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(css.find(".shell-layer-compact .home-composer-mount .prompt-composer-label--thread {\n  display: none;"),
            std::string::npos);
  EXPECT_NE(css.find(".shell-layer-compact .home-composer-mount .prompt-composer-label--home {\n  display: block;"),
            std::string::npos);
}

// The Chats bar: round avatar at the left (Settings), round plus at the right, no title, no large title.
TEST(NewChatPageTest, ChatsBarHasAvatarAndPlusAndNoTitle) {
  const std::string rml = ReadAsset("views/sidebar.rml");
  const size_t bar_end = rml.find("<div class=\"sidebar-header row\">");
  ASSERT_NE(bar_end, std::string::npos);
  const std::string bar = rml.substr(0, bar_end);
  EXPECT_NE(bar.find("open_settings_from_avatar()"), std::string::npos);
  EXPECT_NE(bar.find("open_new_session_menu()"), std::string::npos);
  EXPECT_NE(bar.find("profile_has_icon"), std::string::npos);
  EXPECT_NE(bar.find("{{profile_avatar_letter}}"), std::string::npos);
  EXPECT_EQ(bar.find("pane-nav-title"), std::string::npos);
  EXPECT_EQ(rml.find("large-title"), std::string::npos);
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
  const size_t begin = css.find("\n.nc-page {");
  const size_t end = css.find("/* Contact detail, phone. */");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  const std::string block = css.substr(begin, end - begin);
  EXPECT_EQ(block.find("color:"), std::string::npos);
  EXPECT_EQ(block.find('#'), std::string::npos);
}

} // namespace
} // namespace pbr
