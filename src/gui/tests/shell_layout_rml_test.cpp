#include "gui/shell/ShellLayoutRml.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/ui/ShellLayout.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

bool Has(const std::string& rml, const std::string& part) {
  return rml.find(part) != std::string::npos;
}

size_t Count(const std::string& text, const std::string& part) {
  size_t n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) {
    ++n;
  }
  return n;
}

std::string ReadAsset(const char* relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

ShellState WithChatPane(const LayoutMode mode) {
  ShellState state;
  state.layout_mode = mode;
  state.nav_tab = NavTab::Sessions;
  state.panes.push_back({.spec = {.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true}, .id = 1});
  state.primary_pane_key = "chat";
  return state;
}

TEST(ShellLayoutRmlTest, AnExpandedWindowHasTheRailTheListAndThePrimaryPane) {
  const std::string rml = ShellLayoutRml(WithChatPane(LayoutMode::Expanded), false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "shell-nav-rail-mount"));
  EXPECT_TRUE(Has(rml, "shell-nav-content-mount")) << "Sessions has a list beside the chat";
  EXPECT_TRUE(Has(rml, "id=\"pane-chat\""));
  EXPECT_TRUE(Has(rml, "id=\"pane-composer-chat\"")) << "the chat pane carries the composer";
}

TEST(ShellLayoutRmlTest, ACompactWindowHasTheTabBarAndSettingsIsItsThirdSegment) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = false;
  const std::string chats = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(chats, "id=\"shell-bottom-chrome\""));
  EXPECT_TRUE(Has(chats,
                  "<div class=\"shell-nav-rail shell-nav-rail--compact shell-nav-rail--text\" "
                  "id=\"shell-nav-rail-mount\">"))
      << "the three-word tab bar mounts here";
  EXPECT_FALSE(Has(chats, "shell-nav-page--hidden-tab"));

  // Settings (Me) fills the screen above the tab bar (TODO #200), as a root page of the bar.
  state.nav_tab = NavTab::Me;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "id=\"shell-bottom-chrome\""));
  EXPECT_TRUE(Has(rml, "shell-nav-page--tab-bar")) << "its list clears the bar";
  EXPECT_TRUE(Has(rml, "shell-nav-page--hidden-tab"));
  EXPECT_TRUE(Has(rml, "id=\"pane-body-settings\""));
  EXPECT_FALSE(Has(rml, "account-sheet"));
  EXPECT_FALSE(Has(rml, "close_account_sheet"));
}

// The phone bar is three words (AI, Chats, Settings) and no icons; the desktop rail keeps its four tabs.
TEST(ShellLayoutRmlTest, ThePhoneTabBarHasThreeWordsAndTheDesktopRailFourTabs) {
  const std::string rail = ReadAsset("views/nav_rail.rml");
  const size_t desktop = rail.find("<div class=\"shell-nav-rail-inner\" data-if=\"layout_mode != 'compact'\">");
  ASSERT_NE(desktop, std::string::npos);
  const size_t phone = rail.find("<div class=\"shell-nav-rail-inner\" data-if=\"layout_mode == 'compact'\">");
  ASSERT_NE(phone, std::string::npos);
  ASSERT_LT(phone, desktop);
  const std::string phone_bar = rail.substr(phone, desktop - phone);
  const std::string desktop_rail = rail.substr(desktop);
  EXPECT_EQ(Count(phone_bar, "class=\"shell-nav-tab shell-nav-tab--text\""), 3u);
  EXPECT_EQ(Count(phone_bar, "select_nav_tab("), 3u);
  EXPECT_TRUE(Has(phone_bar, "select_nav_tab('home')"));
  EXPECT_TRUE(Has(phone_bar, "select_nav_tab('sessions')"));
  EXPECT_TRUE(Has(phone_bar, "select_nav_tab('me')"));
  EXPECT_TRUE(Has(phone_bar, "data-class-shell-nav-tab--active=\"nav_bar_tab == 'me'\""));
  EXPECT_TRUE(Has(phone_bar, "{{i18n:nav.home}}"));
  EXPECT_TRUE(Has(phone_bar, "{{i18n:nav.sessions}}"));
  EXPECT_TRUE(Has(phone_bar, "{{i18n:nav.me}}"));
  EXPECT_LT(phone_bar.find("select_nav_tab('sessions')"), phone_bar.find("select_nav_tab('me')")) << "Settings is last";
  EXPECT_FALSE(Has(phone_bar, "<svg")) << "no icons";
  EXPECT_TRUE(Has(phone_bar, "nav_sessions_unread > 0")) << "the Chats unread count stays";
  EXPECT_EQ(Count(desktop_rail, "select_nav_tab("), 4u);
  EXPECT_TRUE(Has(desktop_rail, "select_nav_tab('me')"));
  EXPECT_TRUE(Has(desktop_rail, "select_nav_tab('contacts')"));
}

// The tab bar is drawn on the two tabs' own pages only (DEV/PLAN-minimal-nav.md 目标 8).
TEST(ShellLayoutRmlTest, TheTabBarIsOnlyOnTheAiAndChatsPages) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = false;
  const auto bar_drawn = [&state]() {
    return Has(ShellLayoutRml(state, false, "").ShellRoot(), "shell-bottom-chrome");
  };
  for (const NavTab tab : {NavTab::Home, NavTab::Sessions}) {
    state.nav_tab = tab;
    EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 48.f);
    EXPECT_TRUE(bar_drawn());
  }
  state.nav_tab = NavTab::Contacts;
  EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 0.f);
  EXPECT_FALSE(bar_drawn());
  state.nav_tab = NavTab::Me; // Settings keeps the bar (TODO #200)
  EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 48.f);
  EXPECT_TRUE(bar_drawn());
  state.nav_tab = NavTab::Home;
  state.compact_chat_open = true;
  EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 0.f) << "a chat open";
  EXPECT_FALSE(bar_drawn());
  state.compact_chat_open = false;
  PaneState pushed;
  pushed.spec.key = "new_chat";
  pushed.id = 3;
  state.transient_stack.push_back(pushed);
  // A pushed page covers the bar; the bar stays, so the page's swipe-back shows the page underneath with its bar.
  EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 48.f) << "a pushed page";
  EXPECT_TRUE(bar_drawn());
  state.transient_stack.clear();
  state.layout_mode = LayoutMode::Expanded;
  EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 0.f) << "the desktop has its rail";
  EXPECT_EQ(ShellConfig{}.compact_nav_height_dp, 48.f);
}

// TODO #200: the bar stays on Settings and the pages pushed from it, and only those.
TEST(ShellLayoutRmlTest, SettingsAndItsPushedPagesKeepTheTabBar) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = false;
  state.nav_tab = NavTab::Me;
  const auto root = [&state]() { return ShellLayoutRml(state, false, "").ShellRoot(); };
  const auto push = [&state](const char* key) {
    PaneState pane;
    pane.spec.key = key;
    pane.id = 7;
    state.transient_stack.push_back(pane);
  };

  EXPECT_TRUE(Has(root(), "id=\"shell-bottom-chrome\""));
  EXPECT_FALSE(Has(root(), "shell-bottom-chrome--over-page")) << "nothing pushed";
  for (const char* key : {"settings_detail", "contact_detail", "blocked_people"}) {
    state.transient_stack.clear();
    push(key);
    EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 48.f) << key;
    EXPECT_TRUE(Has(root(), "id=\"shell-bottom-chrome\"")) << key;
    EXPECT_TRUE(Has(root(), "shell-bottom-chrome--over-page")) << key << ": drawn above the pushed page";
  }
  // A page that is not Settings' (a chat's group sheet, the people picker...) keeps no bar.
  state.transient_stack.clear();
  push("contact_detail");
  push("people_picker");
  EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 0.f);
  EXPECT_FALSE(Has(root(), "id=\"shell-bottom-chrome\""));
  state.transient_stack.clear();

  // A secret page (the transfer code) is bare.
  state.secret_page_open = true;
  EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 0.f);
  EXPECT_FALSE(Has(root(), "id=\"shell-bottom-chrome\""));
  state.secret_page_open = false;

  // A chat left open under Settings is not on screen: Settings keeps its bar, and the chat view has none.
  state.compact_chat_open = true;
  EXPECT_TRUE(Has(root(), "id=\"shell-bottom-chrome\""));
  state.nav_tab = NavTab::Sessions;
  EXPECT_TRUE(Has(root(), "shell-chat-overlay"));
  EXPECT_FALSE(Has(root(), "id=\"shell-bottom-chrome\""));

  state.nav_tab = NavTab::Me;
  state.compact_chat_open = false;
  state.layout_mode = LayoutMode::Expanded;
  EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 0.f) << "the desktop has its rail";
  EXPECT_FALSE(Has(root(), "shell-nav-page--tab-bar"));
}

// A hidden tab entered over an open chat (an invite, Settings from the AI) is drawn instead of the chat overlay.
TEST(ShellLayoutRmlTest, AHiddenTabIsDrawnOverAChatLeftOpen) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.nav_tab = NavTab::Contacts;
  state.compact_chat_open = true;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "id=\"pane-body-contacts\""));
  EXPECT_TRUE(Has(rml, "shell-nav-page--hidden-tab"));
  EXPECT_FALSE(Has(rml, "shell-chat-overlay"));
  EXPECT_FALSE(Has(rml, "shell-bottom-chrome"));
}

// DEV/PLAN-minimal-nav.md G: the phone backs are the round button (shell-back-btn stays: the swipe-back's chrome);
// the desktop's pushed page keeps its plain back.
TEST(ShellLayoutRmlTest, ThePhoneBacksAreRoundButtons) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = true;
  const std::string chat = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(chat,
                  "<button class=\"shell-back-btn shell-circle-btn\" type=\"button\" "
                  "data-event-click=\"compact_chat_back()\"><span class=\"shell-circle-btn-face\">"));
  EXPECT_FALSE(Has(chat, "</svg>返回")) << "no word";

  state.compact_chat_open = false;
  PaneState pushed;
  pushed.spec.key = "contact_detail";
  pushed.id = 4;
  state.transient_stack.push_back(pushed);
  EXPECT_TRUE(Has(ShellLayoutRml(state, false, "").ShellRoot(),
                  "<button class=\"shell-back-btn shell-circle-btn\" type=\"button\" "
                  "data-event-click=\"transient_back()\"><span class=\"shell-circle-btn-face\">"));
  state.layout_mode = LayoutMode::Expanded;
  const std::string desktop = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(desktop, "<button class=\"shell-back-btn\" type=\"button\" data-event-click=\"transient_back()\">"));
  EXPECT_FALSE(Has(desktop, "shell-circle-btn"));

  // The views' own backs: Settings' pages, the hidden tabs' roots, the group picker.
  const std::string settings = ReadAsset("views/settings.rml");
  EXPECT_EQ(Count(settings, "class=\"settings-back-btn shell-circle-btn\""), 2u);
  EXPECT_TRUE(Has(settings,
                  "class=\"pane-nav-back shell-circle-btn\" type=\"button\" data-model=\"window\" "
                  "data-if=\"compact_chat_open\" data-event-click=\"hidden_tab_back()\""))
      << "Settings' arrow shows only while a chat thread is open under it";
  EXPECT_TRUE(Has(settings, "{{i18n:settings.title}}"));
  EXPECT_TRUE(Has(ReadAsset("views/contacts.rml"),
                  "class=\"pane-nav-back shell-circle-btn\" type=\"button\" "
                  "data-model=\"window\" data-event-click=\"hidden_tab_back()\""));
  EXPECT_TRUE(Has(ReadAsset("views/people_picker.rml"),
                  "class=\"shell-circle-btn people-picker-back-circle\" type=\"button\" "
                  "data-event-click=\"back_picker()\""));
  // The scanner's back over the camera is the same round button (dark glass), with no text.
  const std::string scan = ReadAsset("views/qr_scan.rml");
  EXPECT_TRUE(Has(scan, "class=\"qr-scan-back shell-circle-btn\" type=\"button\" data-event-click=\"cancel_scan()\""));
  EXPECT_TRUE(Has(scan, "<span class=\"shell-circle-btn-face\"><svg src=\"../icons/v2/back.svg\" width=\"24\""));
  // Every phone back that is a button of its own has no "Back" word beside the round one.
  EXPECT_EQ(Count(ReadAsset("views/settings.rml"), "common.back"), 0u);
  EXPECT_EQ(Count(ReadAsset("views/contacts.rml"), "common.back"), 0u);
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t face = css.find(".shell-layer-compact .shell-circle-btn-face {");
  ASSERT_NE(face, std::string::npos);
  EXPECT_EQ(css.find("width: 44dp;\n  height: 44dp;", face), css.find("width:", face)) << "a 44dp disc";
  const size_t button = css.find(".shell-layer-compact .shell-circle-btn {");
  ASSERT_NE(button, std::string::npos);
  EXPECT_NE(css.find("width: 52dp;\n  height: 52dp;", button), std::string::npos) << "in a 52dp target";
}

// Kenneth 2026-11-03: on Home the dock and the tab bar read as two hard lines; there they are one bottom area.
TEST(ShellLayoutRmlTest, ThePhoneTabBarIs48dpWith16dpWords) {
  // (Kenneth 2026-11-07): one word per segment, the chosen one bold; every segment flexes to an equal share.
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t bar = css.find(
      ".shell-nav-rail--text,\n.shell-nav-rail--text .shell-nav-rail-inner,\n"
      ".shell-nav-rail--text .shell-nav-tab {");
  ASSERT_NE(bar, std::string::npos);
  EXPECT_EQ(css.find("height: 48dp;", bar), css.find("height:", bar));
  const size_t label = css.find(".shell-nav-rail--text .shell-nav-tab-label {");
  ASSERT_NE(label, std::string::npos);
  EXPECT_EQ(css.find("font-size: 16dp;", label), css.find("font-size:", label));
  const size_t active = css.find(".shell-nav-rail--text .shell-nav-tab--active .shell-nav-tab-label {");
  ASSERT_NE(active, std::string::npos);
  EXPECT_EQ(css.find("font-weight: bold;", active), css.find("font-weight:", active));
  // The chosen word sits on a 72 x 32dp rounded pill (its colour is a token in colors-*.rcss).
  const size_t wrap = css.find(
      ".shell-nav-rail--text .shell-nav-tab-text-wrap {\n  width: 72dp;\n  height: 32dp;\n  border-radius: 16dp;");
  EXPECT_NE(wrap, std::string::npos);
  EXPECT_NE(ReadAsset("themes/colors-light.rcss")
                .find(".shell-nav-rail--text .shell-nav-tab--active .shell-nav-tab-text-wrap {"),
            std::string::npos);
}

TEST(ShellLayoutRmlTest, OnlyHomeMarksTheTabBarToBlendWithTheDock) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = false;
  state.nav_tab = NavTab::Home;
  EXPECT_TRUE(Has(ShellLayoutRml(state, false, "").ShellRoot(), "shell-bottom-chrome--home"));
  for (const NavTab tab : {NavTab::Sessions, NavTab::Contacts, NavTab::Me}) {
    state.nav_tab = tab;
    EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").ShellRoot(), "shell-bottom-chrome--home"));
  }
}

// Every phone sheet (Kenneth 2026-11-04): its stop is its class (4/5 = --large, --half and --small also lighten the
// scrim, full has none), and its X is a 44dp button around a small grey disc.
TEST(ShellLayoutRmlTest, ASheetCarriesTheClassOfItsStop) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  OverlayEntry sheet;
  sheet.id = 7;
  sheet.key = "new_chat";
  sheet.page_sheet = true;
  state.overlay_stack.push_back(sheet);
  std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "shell-page-sheet--large")) << "every sheet opens at 4/5";
  EXPECT_FALSE(Has(rml, "--half"));
  EXPECT_TRUE(Has(rml, "<span class=\"sheet-icon-disc\"><svg src=\"../icons/v2/close.svg\" width=\"12\""));
  state.overlay_stack.back().sheet_detent = SheetDetent::Half;
  rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "shell-page-sheet--half"));
  EXPECT_TRUE(Has(rml, "shell-layer-sheet--half"));
  EXPECT_FALSE(Has(rml, "shell-page-sheet--large"));
  state.overlay_stack.back().sheet_detent = SheetDetent::Small;
  rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "shell-page-sheet--small"));
  EXPECT_TRUE(Has(rml, "shell-layer-sheet--small")) << "the scrim lightest at a third";
  EXPECT_FALSE(Has(rml, "--half"));
  state.overlay_stack.back().sheet_detent = SheetDetent::Full;
  rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_FALSE(Has(rml, "--half"));
  EXPECT_FALSE(Has(rml, "--small"));
  EXPECT_FALSE(Has(rml, "--large"));
}

// The phone scanner (Kenneth 2026-11-04, WeChat style): a full-screen layer with no scrim, frame or X of the shell's;
// the view brings its own back. Desktop keeps the centred panel with its close button.
TEST(ShellLayoutRmlTest, AFullScreenLayerHasNoScrimFrameOrCloseButtonOnPhones) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  OverlayEntry scan;
  scan.id = 9;
  scan.key = "qr_scan";
  scan.full_screen = true;
  state.overlay_stack.push_back(scan);
  std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "class=\"shell-layer shell-layer-overlay shell-layer-full-screen\" id=\"shell-full-screen-9\""));
  EXPECT_TRUE(Has(rml, "<div class=\"shell-overlay-body shell-layer-compact\" id=\"overlay-body-9\">"));
  EXPECT_FALSE(Has(rml, "close_layer(9)")) << "no scrim tap or X closes it: the view's back does";
  EXPECT_FALSE(Has(rml, "shell-frame"));
  state.layout_mode = LayoutMode::Expanded;
  rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_FALSE(Has(rml, "shell-layer-full-screen"));
  EXPECT_TRUE(Has(rml, "shell-frame"));
  EXPECT_TRUE(Has(rml, "close_layer(9)"));
}

TEST(ShellLayoutRmlTest, ACompactChatOverlayHasOneHeaderRowHoldingTheBackButton) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = true;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  const size_t chrome = rml.find("shell-chat-overlay-chrome");
  ASSERT_NE(chrome, std::string::npos);
  EXPECT_EQ(rml.find("shell-chat-overlay-chrome", chrome + 1), std::string::npos) << "exactly one bar";
  const size_t back = rml.find("compact_chat_back()", chrome);
  const size_t body = rml.find("id=\"pane-body-chat\"");
  ASSERT_NE(back, std::string::npos);
  ASSERT_NE(body, std::string::npos);
  EXPECT_LT(back, body) << "the back button is in the bar above the chat pane";
  EXPECT_TRUE(Has(rml, "icons/v2/back.svg"));
  // The title, avatar and menu are the chat pane's header row, lifted onto this bar by the theme; the shell
  // root must not draw a second one.
  EXPECT_FALSE(Has(rml, "chat-header"));
}

// #188 / #202: the chat view frosts its header only; its composer bar is clear (a floating pill and two round buttons,
// no material child). A pushed page over the chat leaves no frost of the chat's, and Transparency off leaves none.
TEST(ShellLayoutRmlTest, TheChatViewFrostsItsHeaderAndItsComposerBarIsClear) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = true;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_EQ(Count(rml, "bar-material--frost"), 1U) << "the header only";
  EXPECT_EQ(Count(rml, "class=\"bar-material"), 1U) << "the composer bar has no material child";
  const size_t chrome = rml.find("shell-chat-overlay-chrome");
  const size_t bar = rml.find("shell-composer-bar");
  const size_t mount = rml.find("id=\"shell-composer-mount\"");
  ASSERT_NE(chrome, std::string::npos);
  ASSERT_NE(bar, std::string::npos);
  EXPECT_LT(rml.find("bar-material--frost", chrome), rml.find("back.svg", chrome)) << "the header's material is first";
  ASSERT_NE(mount, std::string::npos);
  EXPECT_EQ(rml.find("bar-material", bar), std::string::npos) << "nothing under the composer mount";
  EXPECT_FALSE(Has(rml, "shell-composer-mount surface-chrome surface-chrome--frost"));

  ShellState solid = state;
  solid.reduce_transparency = true;
  const std::string opaque = ShellLayoutRml(solid, false, "").ShellRoot();
  EXPECT_EQ(Count(opaque, "bar-material--frost"), 0U);
  EXPECT_EQ(Count(opaque, "class=\"bar-material"), 1U) << "the header's material, solid";

  ShellState pushed_over = state;
  PaneState pushed;
  pushed.spec.key = "contact_detail";
  pushed.id = 4;
  pushed_over.transient_stack.push_back(pushed);
  pushed_over.transient_active = true;
  const std::string over = ShellLayoutRml(pushed_over, false, "").ShellRoot();
  EXPECT_EQ(Count(over, "bar-material--frost"), 0U) << "the chat under a pushed page is not frosted";
}

// #176 / #177: New chat and Add contact are titled pushed pages on phones (hide on scroll, frosted once the list is
// under the bar), like the contact page; a desktop window has no such bar.
TEST(ShellLayoutRmlTest, NewChatAndAddContactAreTitledPushedPages) {
  for (const char* key : {"new_chat", "add_contact", "contact_detail"}) {
    ShellState state = WithChatPane(LayoutMode::Compact);
    PaneState pushed;
    pushed.spec.key = key;
    pushed.id = 4;
    state.transient_stack.push_back(pushed);
    state.transient_active = true;
    const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
    EXPECT_TRUE(Has(rml, "shell-layer-transient shell-layer-compact nav-bar-host")) << key;
    EXPECT_TRUE(Has(rml, "shell-transient-chrome--titled")) << key;
    EXPECT_TRUE(Has(rml, "<div class=\"nav-bar-bg\"></div>")) << key;
    EXPECT_TRUE(Has(rml, "surface-chrome--frost")) << key << ": the pushed page's bar is the frosted one";
  }
}

TEST(ShellLayoutRmlTest, TheCompactNavPageFollowsTheLargeTitleState) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = false;
  state.nav_tab = NavTab::Contacts;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "id=\"shell-nav-page\" data-class-large-title-collapsed=\"large_title_collapsed\""));
}

TEST(ShellLayoutRmlTest, AnActiveDialogIsRenderedByItsMountOnlyNotInlineInTheRoot) {
  // A full layout sync while a dialog is open rebuilds the root and then fills the dialog mount;
  // an inline copy in the root would survive dialog_close as a dead OK button over the whole app.
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Confirm;
  state.dialog.show_cancel = true;
  const ShellLayoutRml rml(state, false, "");
  EXPECT_TRUE(Has(rml.Dialog(), "shell-layer-dialog"));
  EXPECT_FALSE(Has(rml.ShellRoot(), "shell-layer-dialog"));
  EXPECT_TRUE(Has(rml.ShellRoot(), "id=\"shell-dialog-mount\""));
}

TEST(ShellLayoutRmlTest, ADestructiveConfirmCarriesTheDangerClassAndASplitButtonPair) {
  // The buttons' default labels are words ("OK", "Cancel"); with no strings loaded they would be the long keys and
  // stack, so the result would depend on which test loaded the strings first.
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Confirm;
  state.dialog.show_cancel = true;
  const std::string plain = ShellLayoutRml(state, false, "").Dialog();
  EXPECT_FALSE(Has(plain, "shell-dialog--destructive"));
  EXPECT_TRUE(Has(plain, "shell-dialog-ok shell-dialog-ok--split"));
  EXPECT_FALSE(Has(plain, "shell-dialog-actions--stack"));

  state.dialog.destructive = true;
  EXPECT_TRUE(Has(ShellLayoutRml(state, false, "").Dialog(), "shell-dialog--destructive"));

  // An alert has the one button, with no vertical hairline beside it.
  state.dialog.show_cancel = false;
  EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").Dialog(), "shell-dialog-ok--split"));
}

TEST(ShellLayoutRmlTest, ALongButtonLabelStacksTheDialogButtons) {
  EXPECT_FALSE(ShellLayoutRml::DialogButtonsStacked("Cancel", "Delete"));
  EXPECT_FALSE(ShellLayoutRml::DialogButtonsStacked("取消", "移除联系人"));
  EXPECT_TRUE(ShellLayoutRml::DialogButtonsStacked("Cancel", "Remove this device"));
  EXPECT_TRUE(ShellLayoutRml::DialogButtonsStacked("取消", "移除这台设备并退出登录"));

  ShellState state = WithChatPane(LayoutMode::Compact);
  state.dialog.active = true;
  state.dialog.show_cancel = true;
  state.dialog.ok_label = "Remove this device";
  EXPECT_TRUE(Has(ShellLayoutRml(state, false, "").Dialog(), "shell-dialog-actions--stack"));
}

// The unlock card is frosted glass with Transparency on (material child first) and the plain solid card with it off.
TEST(ShellLayoutRmlTest, ThePinGateCardIsFrostedOnlyWithTransparencyOn) {
  ShellState state;
  state.pin_gate.active = true;
  state.pin_gate.unlock_mode = true;
  state.compact_chrome_frost = true;
  const std::string frosted = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_TRUE(Has(frosted, "shell-pin-gate shell-pin-gate--frost"));
  EXPECT_EQ(Count(frosted, "bar-material--frost"), 1U);
  EXPECT_LT(frosted.find("bar-material--frost"), frosted.find("shell-dialog-title")) << "the material is first";

  state.reduce_transparency = true;
  const std::string solid = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_FALSE(Has(solid, "shell-pin-gate--frost"));
  EXPECT_FALSE(Has(solid, "bar-material"));
}

// Over the "Incoming call, connecting…" page the unlock layer is marked so its scrim goes clear and the card steps aside.
TEST(ShellLayoutRmlTest, ThePinGateOverTheAwaitingCallPageCarriesTheOverCallClass) {
  ShellState state;
  state.pin_gate.active = true;
  state.pin_gate.unlock_mode = true;
  state.compact_chrome_frost = true;
  EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").PinGate(), "shell-pin-gate--over-call"));
  state.call_ring.active = true;
  state.call_ring.awaiting_invite = true;
  const std::string over = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_TRUE(Has(over, "shell-layer shell-layer-dialog shell-pin-gate--over-call\""));
  EXPECT_LT(over.find("shell-pin-gate--over-call"), over.find("shell-scrim")) << "on the layer, above the scrim";
  EXPECT_TRUE(Has(over, "shell-pin-gate shell-pin-gate--frost")) << "the frosted card is unchanged";
  state.call_ring.awaiting_invite = false;
  EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").PinGate(), "shell-pin-gate--over-call"))
      << "a real ring: as before";
}

// DEV/PLAN-pin-gate-redesign.md B: the number pad has no input element (no system keyboard, the PIN bound nowhere);
// the field mode keeps its field, which takes the focus as the gate comes up (Kenneth 2026-11-07).
TEST(ShellLayoutRmlTest, ThePinPadHasNoFieldAndTheFieldModeFocusesItsField) {
  ShellState state;
  state.pin_gate.active = true;
  state.pin_gate.unlock_mode = true;
  state.pin_gate.keypad = true;
  const std::string pad = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_FALSE(Has(pad, "<input"));
  EXPECT_FALSE(Has(pad, "pin_gate_pin"));
  EXPECT_FALSE(Has(pad, "pin_gate_submit()")) << "a full PIN submits itself";
  for (int digit = 0; digit <= 9; ++digit) {
    EXPECT_TRUE(Has(pad, "pin_gate_key(" + std::to_string(digit) + ")")) << digit;
  }
  EXPECT_TRUE(Has(pad, "pin_gate_backspace()"));
  EXPECT_TRUE(Has(pad, "pin_gate_ok()"));
  EXPECT_TRUE(Has(pad, "pin_gate_use_text()")) << "the field is always one tap away";
  EXPECT_FALSE(ShellLayoutRml::PinGateFocusesField(state.pin_gate));

  state.pin_gate.keypad = false;
  const std::string field = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_TRUE(Has(field, std::string("id=\"") + ShellLayoutRml::kPinGateFieldId + "\""));
  EXPECT_TRUE(Has(field, "data-value=\"pin_gate_pin\""));
  EXPECT_FALSE(Has(field, "pin_gate_key("));
  EXPECT_TRUE(Has(field, "pin_gate_use_keypad()"));
  EXPECT_TRUE(ShellLayoutRml::PinGateFocusesField(state.pin_gate));

  for (bool PinGateState::* mode :
       {&PinGateState::chooser_mode, &PinGateState::identity_fork_mode, &PinGateState::link_paste_mode,
        &PinGateState::waiting_device_unlock, &PinGateState::biometric_first}) {
    PinGateState gate = state.pin_gate;
    gate.*mode = true;
    EXPECT_FALSE(ShellLayoutRml::PinGateFocusesField(gate)) << "no PIN field to type in there";
  }
  PinGateState closed;
  EXPECT_FALSE(ShellLayoutRml::PinGateFocusesField(closed));
}

// Face / fingerprint unlock (DEV/PLAN-pin-gate-redesign.md C): the pad's bottom-left key (where no "OK" is due) with
// the face or the fingerprint icon; the field's link by the sensor's name; the hint line when there is one.
TEST(ShellLayoutRmlTest, TheBiometricKeyLinkAndHintFollowTheirFields) {
  ShellState state;
  state.pin_gate.active = true;
  state.pin_gate.unlock_mode = true;
  state.pin_gate.keypad = true;
  const std::string pad = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_TRUE(Has(pad,
                  "data-if=\"pin_gate_biometric_key && !pin_gate_keypad_ok_shown\" "
                  "data-event-click=\"pin_gate_biometric()\""));
  EXPECT_TRUE(Has(pad, "data-if=\"!pin_gate_keypad_ok_shown && !pin_gate_biometric_key\"")) << "else the blank";
  EXPECT_TRUE(Has(pad, "data-if=\"pin_gate_biometric_kind == 'face'\" src=\"../icons/v2/face-id.svg\""));
  EXPECT_TRUE(Has(pad, "data-if=\"pin_gate_biometric_kind != 'face'\" src=\"../icons/v2/fingerprint.svg\""));
  EXPECT_TRUE(Has(pad, "data-if=\"pin_gate_biometric_hint != ''\">{{pin_gate_biometric_hint}}"));
  for (const char* icon : {"icons/v2/face-id.svg", "icons/v2/fingerprint.svg"}) {
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / icon)) << icon;
  }

  state.pin_gate.keypad = false;
  const std::string field = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_FALSE(Has(field, "face-id.svg"));
  for (const char* kind : {"face", "touch", "fingerprint"}) {
    EXPECT_TRUE(Has(field, std::string("pin_gate_biometric_kind == '") + kind + "'")) << kind;
    EXPECT_TRUE(Has(field, Tr(std::string("pin.biometric_link.") + kind))) << kind;
  }
  EXPECT_TRUE(Has(field, "pin_gate_biometric()"));
  EXPECT_TRUE(Has(field, "{{pin_gate_biometric_hint}}"));
}

// Face / fingerprint first: the door opens with the big sensor key and "Enter PIN" -- no pad, no field.
TEST(ShellLayoutRmlTest, ABiometricFirstDoorHasOnlyTheKeyAndEnterPin) {
  ShellState state;
  state.pin_gate.active = true;
  state.pin_gate.unlock_mode = true;
  state.pin_gate.keypad = true;
  state.pin_gate.biometric_first = true;
  const std::string door = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_TRUE(Has(door, "class=\"shell-pin-bio-key\" data-event-click=\"pin_gate_biometric()\""));
  EXPECT_TRUE(Has(door, "data-if=\"pin_gate_biometric_kind == 'face'\" src=\"../icons/v2/face-id.svg\""));
  EXPECT_TRUE(Has(door, "data-if=\"pin_gate_biometric_kind != 'face'\" src=\"../icons/v2/fingerprint.svg\""));
  EXPECT_TRUE(Has(door, "data-event-click=\"pin_gate_use_pin()\">" + Tr("pin.biometric_use_pin")));
  EXPECT_FALSE(Has(door, "shell-pin-keys")) << "no pad yet";
  EXPECT_FALSE(Has(door, "pin_gate_key("));
  EXPECT_FALSE(Has(door, std::string("id=\"") + ShellLayoutRml::kPinGateFieldId)) << "no field yet";
  EXPECT_TRUE(Has(door, "pin_gate_forgot_toggle()")) << "Forgot PIN stays";
}

TEST(ShellLayoutRmlTest, ThePinPadKeysAreAtLeast64dp) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "themes/components.rcss");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string css = buffer.str();
  const size_t key = css.find(".shell-pin-key {");
  ASSERT_NE(key, std::string::npos);
  EXPECT_EQ(css.find("width: 72dp;\n  height: 72dp;", key), css.find("width:", key));
}

TEST(ShellLayoutRmlTest, NoDialogNoPinGateNoMarkup) {
  const ShellLayoutRml rml(ShellState{}, false, "");
  EXPECT_TRUE(rml.Dialog().empty());
  EXPECT_TRUE(rml.PinGate().empty());
  EXPECT_TRUE(rml.BottomChrome().empty());
}

} // namespace
} // namespace pbr
