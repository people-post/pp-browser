// Phone "New chat" page: the markup binds what ContactsController provides, and its rows have the agreed sizes.
#include "lib/ui/ViewCatalog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

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

// #176 / #177: the list scrolls under the pushed page's frosted bar: the scroller is marked for ShellNavBarScroll
// (which sets nav-bar-scrolled / nav-bar-hidden on the layer) and the stylesheet lays it over the whole page.
TEST(NewChatPageTest, TheListScrollsUnderTheStickyBar) {
  for (const auto& [view, scroller] : {std::pair<const char*, const char*>{"views/new_chat.rml", "nc-scroll"},
                                       {"views/add_contact.rml", "ac-scroll"}}) {
    const std::string rml = ReadAsset(view);
    EXPECT_NE(rml.find(std::string("class=\"") + scroller + " nav-bar-scroll\""), std::string::npos) << view;
  }
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t rule = css.find(".shell-layer-compact .nc-page .nc-scroll,\n.shell-layer-compact .ac-page .ac-scroll {");
  ASSERT_NE(rule, std::string::npos);
  const std::string body = css.substr(rule, css.find('}', rule) - rule);
  EXPECT_NE(body.find("position: absolute"), std::string::npos);
  EXPECT_NE(body.find("padding-top: 56dp"), std::string::npos) << "starts below the 56dp bar";
  EXPECT_NE(css.find(".shell-layer-transient.nav-bar-host.nav-bar-hidden .nc-bar {"), std::string::npos)
      << "the title row slides away with the bar";
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
  for (const char* event :
       {"data-event-mousedown=\"contacts_index_at('new_chat')\"", "data-event-drag=\"contacts_index_at('new_chat')\"",
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
  EXPECT_NE(
      composer.find("prompt-composer-label--thread\" data-if=\"draft == '' && thread_is_ai\">{{draft_placeholder}}"),
      std::string::npos);
  EXPECT_NE(composer.find("{{i18n:home.composer.placeholder}}"), std::string::npos);
  // The Home-root label must not wait for a thread: no thread is open there, thread_is_ai is false.
  EXPECT_NE(composer.find("prompt-composer-label--home\" data-if=\"draft == ''\">{{i18n:home.composer.placeholder}}"),
            std::string::npos);
  // The Home field's tools sit inside the pill: the bold plus (opens the Camera / Photos menu) is a child of the pill,
  // and the emoji key is positioned inside the row, which is the pill's rectangle.
  const size_t pill = composer.find("class=\"prompt-composer-pill\"");
  const size_t plus = composer.find("class=\"prompt-home-plus\"");
  const size_t pill_end = composer.find("<!-- Voice mode (WeChat style)");
  ASSERT_NE(pill, std::string::npos);
  ASSERT_NE(plus, std::string::npos);
  EXPECT_TRUE(plus > pill && plus < pill_end) << "the plus is inside the pill";
  EXPECT_NE(composer.find("data-event-click=\"home_attach_menu()\"", plus), std::string::npos);
  EXPECT_NE(composer.find("src=\"../icons/v2/plus-bold.svg\"", plus), std::string::npos);
  EXPECT_NE(composer.find("class=\"prompt-emoji-btn\""), std::string::npos);
  EXPECT_NE(en.find("\"home.attach.camera\": \"Camera\""), std::string::npos);
  EXPECT_NE(en.find("\"home.attach.photo\": \"Photos\""), std::string::npos);
  EXPECT_NE(zh.find("\"home.attach.camera\": \"相机\""), std::string::npos);
  EXPECT_NE(zh.find("\"home.attach.photo\": \"图片\""), std::string::npos);
  // The Home label shows only inside the Home mount; the thread one hides there.
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(css.find(".shell-layer-compact .home-composer-mount .prompt-composer-label--thread {\n  display: none;"),
            std::string::npos);
  EXPECT_NE(css.find(".shell-layer-compact .home-composer-mount .prompt-composer-label--home {\n  display: block;"),
            std::string::npos);
  // Card B3: the plus (a 44dp target) at the bottom left; the emoji key right before Send (10dp gap) is a 44dp target
  // whose icon carries the 32dp tinted circle; Send is always at the right end (disabled grey while empty).
  const size_t plus_rule =
      css.find(".shell-layer-compact .home-composer-mount .prompt-composer-pill .prompt-home-plus {");
  ASSERT_NE(plus_rule, std::string::npos);
  EXPECT_NE(css.find("left: 4dp;", plus_rule), std::string::npos);
  EXPECT_NE(css.find("width: 44dp;\n  height: 44dp;", plus_rule), std::string::npos);
  const size_t emoji_rule =
      css.find(".shell-layer-compact .home-composer-mount .prompt-composer-row .prompt-emoji-btn {");
  ASSERT_NE(emoji_rule, std::string::npos);
  EXPECT_NE(css.find("right: 50dp;", emoji_rule), std::string::npos);
  EXPECT_NE(css.find("width: 44dp;\n  height: 44dp;", emoji_rule), std::string::npos);
  const size_t emoji_icon =
      css.find(".shell-layer-compact .home-composer-mount .prompt-emoji-btn .prompt-icon-home-emoji {");
  ASSERT_NE(emoji_icon, std::string::npos);
  EXPECT_NE(css.find("width: 32dp;\n  height: 32dp;", emoji_icon), std::string::npos);
  EXPECT_NE(css.find("border-radius: 16dp;", emoji_icon), std::string::npos);
  EXPECT_NE(composer.find("src=\"../icons/v2/emoji-round.svg\""), std::string::npos);
  const size_t send_rule =
      css.find(".shell-layer-compact .home-composer-mount .prompt-composer-row .prompt-send-btn {");
  ASSERT_NE(send_rule, std::string::npos);
  EXPECT_NE(css.find("right: 10dp;", send_rule), std::string::npos);
  EXPECT_NE(composer.find("prompt-send-btn prompt-send-btn--idle"), std::string::npos)
      << "the idle Send is on the Home card too";
  EXPECT_NE(css.find(".shell-layer-compact .home-composer-mount .prompt-voice-toggle-btn {\n  display: none;"),
            std::string::npos)
      << "no microphone on the Home card";
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

// Mockup screen 4: two side-by-side cards, then white rounded groups of 56dp rows with 40dp avatars under grey letters.
TEST(NewChatPageTest, PageIsTheGroupedLookWithTwoCardsAndRoundedGroups) {
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t actions = css.find(".shell-layer-compact .nc-page .nc-actions {");
  ASSERT_NE(actions, std::string::npos);
  EXPECT_NE(css.find("flex-direction: row;", actions), std::string::npos);
  EXPECT_NE(css.find("gap: 12dp;", actions), std::string::npos);
  const size_t card = css.find(".shell-layer-compact .nc-page .nc-action {");
  ASSERT_NE(card, std::string::npos);
  EXPECT_NE(css.find("border-radius: 14dp;", card), std::string::npos);
  const size_t group = css.find(".shell-layer-compact .nc-page .nc-group {");
  ASSERT_NE(group, std::string::npos);
  EXPECT_NE(css.find("margin: 0 16dp;", group), std::string::npos);
  const size_t row = css.find(".shell-layer-compact .nc-page .contacts-row {");
  ASSERT_NE(row, std::string::npos);
  EXPECT_NE(css.find("height: 56dp;", row), std::string::npos);
  EXPECT_NE(css.find("width: 40dp;", css.find(".shell-layer-compact .nc-page .contacts-row-avatar-frame {")),
            std::string::npos);
  const std::string rml = ReadAsset("views/new_chat.rml");
  EXPECT_EQ(rml.find("new_friends"), std::string::npos) << "no new-friends row";
  EXPECT_NE(rml.find("class=\"nc-group\""), std::string::npos);
}

// The Chats bar is the large-title layout with your avatar in place of the words: the small bar has your small avatar
// (shown once collapsed) and the compose button; the title row has your avatar button; no name anywhere.
TEST(NewChatPageTest, ChatsBarIsTheLargeTitleLayout) {
  const std::string rml = ReadAsset("views/sidebar.rml");
  const size_t bar_end = rml.find("<div class=\"sidebar-header row\">");
  const std::string bar = rml.substr(0, bar_end);
  EXPECT_NE(bar.find("class=\"pane-nav-me\""), std::string::npos);
  EXPECT_EQ(bar.find("class=\"pane-nav-title\""), std::string::npos);
  EXPECT_NE(bar.find("open_new_session_menu()"), std::string::npos);
  EXPECT_EQ(bar.find("pane-nav-avatar"), std::string::npos);
  EXPECT_EQ(bar.find("{{profile_nickname}}"), std::string::npos);
  EXPECT_NE(rml.find("<div class=\"large-title\">"), std::string::npos);
  EXPECT_NE(rml.find("class=\"chat-me\""), std::string::npos);
  EXPECT_EQ(rml.find("<p class=\"large-title\">"), std::string::npos);
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
