#include "gui/shell/ShellLayoutRml.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

bool Has(const std::string& rml, const std::string& part) {
  return rml.find(part) != std::string::npos;
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

TEST(ShellLayoutRmlTest, ACompactWindowHasTheTabBarAndMeIsAPlainTab) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = false;
  state.nav_tab = NavTab::Me;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "id=\"shell-bottom-chrome\""));
  EXPECT_TRUE(Has(rml, "id=\"shell-nav-rail-mount\"")) << "the tab bar mounts here";
  EXPECT_TRUE(Has(rml, "id=\"pane-body-settings\"")) << "Me fills the page like any other tab";
  EXPECT_FALSE(Has(rml, "account-sheet"));
  EXPECT_FALSE(Has(rml, "close_account_sheet"));
}

// Kenneth 2026-11-03: on Home the dock and the tab bar read as two hard lines; there they are one bottom area.
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

// The ask-AI panel (DEV/PLAN-ui-batch5-ask-ai.md 5a): a half-stop sheet at half carries the half classes (its top at
// mid-screen, a lighter scrim); at full, or a plain sheet, does not.
TEST(ShellLayoutRmlTest, AHalfStopSheetAtHalfIsMarkedHalf) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  OverlayEntry sheet;
  sheet.id = 7;
  sheet.key = "ask_ai";
  sheet.page_sheet = true;
  sheet.sheet_half_stop = true;
  sheet.sheet_detent = SheetDetent::Half;
  state.overlay_stack.push_back(sheet);
  std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "shell-page-sheet--half"));
  EXPECT_TRUE(Has(rml, "shell-layer-sheet--half"));
  state.overlay_stack.back().sheet_detent = SheetDetent::Full;
  rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_FALSE(Has(rml, "shell-page-sheet--half"));
  EXPECT_FALSE(Has(rml, "shell-layer-sheet--half"));
  state.overlay_stack.back().sheet_half_stop = false;
  state.overlay_stack.back().sheet_detent = SheetDetent::Half;
  EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").ShellRoot(), "--half")) << "no half stop: always full";
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

TEST(ShellLayoutRmlTest, NoDialogNoPinGateNoMarkup) {
  const ShellLayoutRml rml(ShellState{}, false, "");
  EXPECT_TRUE(rml.Dialog().empty());
  EXPECT_TRUE(rml.PinGate().empty());
  EXPECT_TRUE(rml.BottomChrome().empty());
}

} // namespace
} // namespace pbr
